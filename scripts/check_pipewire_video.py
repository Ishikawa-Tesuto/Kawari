#!/usr/bin/env python3
"""Inspect video nodes and incident links; existence does not prove frame delivery."""
import json
import sys

def report(objects):
    nodes = {o['id']: o for o in objects if o.get('type') == 'PipeWire:Interface:Node'}
    links = [o for o in objects if o.get('type') == 'PipeWire:Interface:Link']
    def props(o): return (o.get('info') or {}).get('props') or {}
    def name(id): return props(nodes.get(id, {})).get('node.name', str(id))
    count = 0
    matches = 0
    for id, node in sorted(nodes.items()):
        p = props(node)
        cls = str(p.get('media.class', ''))
        combined = ' '.join(str(p.get(k,'')) for k in ('node.name','node.description','media.name','application.name','application.id')).lower()
        match = 'test stream' in combined or 'libfunnel.test-vk' in combined or 'funnel vulkan test' in combined
        if 'video' not in cls.lower() and not match:
            continue
        count += 1
        matches += int(match)
        info = node.get('info') or {}
        print(f"{'[PROBE] ' if match else ''}id={id} state={info.get('state','?')} class={cls or '?'} name={name(id)}")
        for key in ('node.description','media.name','application.name','application.id'):
            if key in p: print(f'  {key}: {p[key]}')
        for link in links:
            li = link.get('info') or {}
            src, dst = li.get('output-node-id'), li.get('input-node-id')
            if id in (src,dst):
                print(f"  link {link['id']}: {name(src)} -> {name(dst)} ({li.get('state','?')})")
    print(f'Video/probe nodes: {count}; matching test nodes: {matches}')
    if not matches:
        print('No named test node detected. Keep the demo running; check the video node list above.')
    else:
        print('Node detected. Confirm actual animation and alpha in OBS; registration alone is not a frame test.')
    return 0 if matches else 1

if __name__ == '__main__':
    try:
        sys.exit(report(json.load(sys.stdin)))
    except (ValueError, TypeError, KeyError) as exc:
        print(f'Cannot parse pw-dump: {exc}', file=sys.stderr)
        sys.exit(2)
