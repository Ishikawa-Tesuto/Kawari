#!/usr/bin/env python3
"""Development-only local parameter sender; no network service or VTS protocol."""
import argparse
import math
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket', help='path printed by [parameters] socket:')
    parser.add_argument('--source', choices=['test', 'plugin', 'tracking'], default='test')
    parser.add_argument('--ttl', type=int, default=500, choices=range(50, 5001), metavar='50..5000')
    actions = parser.add_subparsers(dest='action', required=True)
    setter = actions.add_parser('set')
    setter.add_argument('pairs', nargs='+', metavar='ID=VALUE')
    sine = actions.add_parser('sine')
    sine.add_argument('id')
    sine.add_argument('--amplitude', type=float, default=25)
    sine.add_argument('--center', type=float, default=0)
    sine.add_argument('--period', type=float, default=4)
    sine.add_argument('--seconds', type=float, default=15)
    sine.add_argument('--hz', type=int, default=60, choices=range(1, 121), metavar='1..120')
    actions.add_parser('clear')
    args = parser.parse_args()
    def pair(name, value):
        if not name or any(c.isspace() for c in name) or not math.isfinite(value):
            parser.error('expected an input ID and finite numeric value')
        return f'{name} {value:.8g}'
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.2)
        sock.connect(args.socket)
        def send(message):
            sock.send(message.encode('utf-8'))
        if args.action == 'clear':
            send(f'clear {args.source}')
        elif args.action == 'set':
            try:
                pairs = [p.split('=', 1) for p in args.pairs]
                payload = ' '.join(pair(k, float(v)) for k, v in pairs)
            except ValueError:
                parser.error('expected ID=VALUE pairs')
            send(f'set {args.source} {args.ttl} {payload}')
        else:
            if not all(math.isfinite(v) for v in (args.amplitude,args.center,args.period,args.seconds)) or args.period <= 0 or args.seconds <= 0:
                parser.error('period and duration must be positive finite numbers')
            start = time.monotonic()
            try:
                while (elapsed := time.monotonic() - start) < args.seconds:
                    v = args.center + args.amplitude * math.sin(elapsed * math.tau / args.period)
                    send(f'set {args.source} {args.ttl} {pair(args.id,v)}')
                    time.sleep(1 / args.hz)
            except KeyboardInterrupt:
                pass
            finally:
                send(f'clear {args.source}')


if __name__ == '__main__':
    try:
        main()
    except OSError as exc:
        raise SystemExit(f'Parameter socket: {exc}')
