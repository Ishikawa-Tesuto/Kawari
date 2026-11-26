#!/usr/bin/env python3
"""Single-entry launcher for the tested native viewer and VTS bridge."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import stat
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def prepare(args):
    model = args.model.resolve(strict=True)
    if not model.is_file() or not model.name.endswith('.model3.json'):
        raise ValueError('Expected an original .model3.json file')
    key = hashlib.sha256(str(model).encode()).hexdigest()[:24]
    home = Path(os.environ.get('XDG_DATA_HOME') or Path.home()/'.local/share')
    profile = (args.profile_dir or home/'vtuber-studio'/'profiles'/key).resolve()
    if profile.is_relative_to(model.parent):
        raise ValueError('Studio profile must be outside original model directory')
    profile.mkdir(mode=0o700, parents=True, exist_ok=True)
    lock = (profile/'session.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        config = model.with_name(model.name.removesuffix('.model3.json')+'.vtube.json')
        runtime = model
        if args.runtime_model:
            runtime = args.runtime_model.resolve(strict=True)
            if runtime != model:
                report = json.loads((runtime.parent/'studio-profile.json').read_text())
                if Path(report['sourceModel']).resolve() != model or runtime.name != model.name:
                    raise ValueError('Runtime profile belongs to another model')
        elif config.exists():
            dest = profile/'runtime'
            if not dest.exists():
                subprocess.run([sys.executable, str(ROOT/'scripts/prepare-model.py'), str(model), '--output-dir', str(dest)], check=True)
            report = json.loads((dest/'studio-profile.json').read_text())
            if Path(report['sourceModel']).resolve() != model:
                raise ValueError('Existing runtime profile belongs to another model')
            runtime = dest/model.name
        mapping = args.parameter_map.resolve(strict=True) if args.parameter_map else None
        if mapping is None and config.exists():
            mapping = profile/'parameters.map'
            if not mapping.exists():
                subprocess.run([sys.executable, str(ROOT/'scripts/import-vtube.py'), str(config), '--output', str(mapping)], check=True)
        state = (args.state_file or profile/'state.json').resolve()
        if state.is_relative_to(model.parent):
            raise ValueError('State must be outside original model directory')
        tokens = (profile/'api-tokens.json').resolve()
        if state == tokens or (mapping and state == mapping.resolve()):
            raise ValueError('State file conflicts with tokens or parameter map')
        if tokens.is_relative_to(model.parent):
            raise ValueError('Tokens must be outside original model directory')
        return model, runtime, mapping, state, profile/'api-tokens.json', lock
    except BaseException:
        lock.close()
        raise


def spawn(command, events, label):
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=None,
                             text=True, errors='replace', bufsize=1, start_new_session=True)
    def relay():
        with child.stdout:
            for line in child.stdout:
                print(line, end='', flush=True)
                if label == 'viewer' and line.startswith('[parameters] socket: '):
                    events.put(('socket', line.removeprefix('[parameters] socket: ').strip()))
                elif label == 'api' and line.startswith('[vts-api] Listening on '):
                    events.put(('ready', None))
    threading.Thread(target=relay, daemon=True).start()
    return child


def stop_child(child):
    if child is None or child.poll() is not None:
        return
    # Each child is its own process group; no process-name matching or unrelated kills.
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try: os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        child.wait()


def wait_event(events, kind, children, stopping, timeout):
    deadline = time.monotonic()+timeout
    while not stopping.is_set():
        for label, child in children:
            if child.poll() is not None:
                raise RuntimeError(f'{label} exited before ready (status {child.returncode})')
        if time.monotonic() >= deadline:
            raise RuntimeError(f'Timed out waiting for {kind}')
        try: name, value = events.get(timeout=.1)
        except queue.Empty: continue
        if name == kind: return value
    raise InterruptedError('Stopped')


def wait_snapshot(path, viewer, stopping, timeout):
    deadline = time.monotonic()+timeout
    while not stopping.is_set():
        if viewer.poll() is not None: raise RuntimeError('Viewer exited before snapshot')
        try:
            st = path.stat(); parent = path.parent.stat()
            if not stat.S_ISSOCK(st.st_mode) or st.st_uid != os.getuid() or parent.st_uid != os.getuid() or parent.st_mode & 0o077:
                raise ValueError('Unexpected parameter socket ownership or permissions')
            snap = path.parent/'parameters.tsv'
            if time.time()-snap.stat().st_mtime < 3 and any(row.startswith('P\t') for row in snap.read_text().splitlines()):
                return
        except FileNotFoundError: pass
        if time.monotonic() >= deadline: raise RuntimeError('Timed out waiting for model parameters')
        stopping.wait(.1)
    raise InterruptedError('Stopped')


def supervise(viewer_cmd, api_cmd, stopping, timeout=30):
    events = queue.Queue(); viewer = api = None
    try:
        viewer = spawn(viewer_cmd, events, 'viewer')
        path = Path(wait_event(events, 'socket', [('Viewer', viewer)], stopping, timeout))
        wait_snapshot(path, viewer, stopping, timeout)
        api = spawn([*api_cmd, '--socket', str(path)], events, 'api')
        wait_event(events, 'ready', [('Viewer', viewer), ('API', api)], stopping, timeout)
        print('[studio] Ready: renderer + VTS API', flush=True)
        while not stopping.wait(.1):
            if viewer.poll() is not None: return viewer.returncode if viewer.returncode >= 0 else 1
            if api.poll() is not None: raise RuntimeError(f'API exited unexpectedly ({api.returncode})')
        return 0
    except InterruptedError:
        return 0
    finally:
        stop_child(api)
        stop_child(viewer)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('model', type=Path, help='original .model3.json, not prepared runtime copy')
    p.add_argument('--profile-dir', type=Path)
    p.add_argument('--runtime-model', type=Path, help='reuse a prepared P0.4c model')
    p.add_argument('--parameter-map', type=Path, help='reuse an existing mapping')
    p.add_argument('--state-file', type=Path, help='reuse an existing expression state')
    p.add_argument('--allow-plugin', action='append', default=[], help='explicit plugin name allowlist; repeat as needed')
    p.add_argument('--port', type=int, default=8001)
    p.add_argument('--api-backend', choices=('python','rust'), default='python', help='select tested Python or experimental native Rust API')
    p.add_argument('--size', default='800x600'); p.add_argument('--broadcast-size', default='1920x1080')
    p.add_argument('--broadcast-fps', type=int, choices=(30,60,120,144), default=60)
    p.add_argument('--fps', type=int, choices=(0,30,60,120,144), default=60)
    p.add_argument('--no-preview', action='store_true'); p.add_argument('--no-broadcast', action='store_true')
    p.add_argument('--stats', action='store_true'); p.add_argument('--startup-timeout', type=float, default=30)
    args = p.parse_args()
    if not 1 <= args.port <= 65535 or not 1 <= args.startup_timeout <= 300: p.error('Invalid port or startup timeout')
    if args.no_preview and args.no_broadcast: p.error('--no-preview requires broadcast')
    if not args.allow_plugin: p.error('Specify at least one --allow-plugin NAME (e.g. VBridger)')
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, lambda *_: stop.set())
    lock = None
    try:
        api_bin = ROOT/'api-rust/target/release/studio-vts-api'
        if args.api_backend == 'rust' and not (api_bin.is_file() and os.access(api_bin, os.X_OK)):
            raise ValueError('Rust API not built: run scripts/build-api.sh')
        model, runtime, mapping, state, tokens, lock = prepare(args)
        if stop.is_set(): return 0
        viewer = [str(ROOT/'scripts/run.sh'), '--parameter-input', '--size', args.size, '--fps', str(args.fps)]
        if not args.no_broadcast:
            viewer += ['--no-preview' if args.no_preview else '--broadcast', '--broadcast-size', args.broadcast_size, '--broadcast-fps', str(args.broadcast_fps)]
        if args.stats: viewer += ['--stats']
        if mapping: viewer += ['--parameter-map', str(mapping)]
        viewer += [str(runtime)]
        api = ([str(api_bin)] if args.api_backend == 'rust' else [sys.executable, '-u', str(ROOT/'scripts/vts-api.py')])
        api += ['--model', str(model), '--port', str(args.port), '--restore-expressions', '--state-file', str(state), '--token-file', str(tokens)]
        print(f'[studio] API backend: {args.api_backend}', flush=True)
        for name in args.allow_plugin: api += ['--allow-plugin', name]
        return supervise(viewer, api, stop, args.startup_timeout)
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as exc:
        print(f'[studio] {exc}', file=sys.stderr); return 1
    finally:
        if lock: lock.close()

if __name__ == '__main__': sys.exit(main())
