#!/usr/bin/env bash
set -euo pipefail
command -v pw-dump >/dev/null || { echo 'pw-dump missing' >&2; exit 2; }
# Show only the studio's own source and its connected consumers, not the demo.
pw-dump | python3 -c '
import json,sys
objects=json.load(sys.stdin)
nodes={o["id"]:o for o in objects if o.get("type")=="PipeWire:Interface:Node"}
def info(o): return o.get("info") or {}
def props(o): return info(o).get("props") or {}
def name(i): return props(nodes.get(i,{})).get("node.name",str(i))
matches=[]
for i,o in nodes.items():
 p=props(o); n=name(i); cl=str(p.get("media.class","")); desc=str(p.get("node.description",""))
 if "Stream/Output/Video" in cl and ("ru.tesutoidclub.vtuber-studio" in n or (("Kawari" in desc or "VTuber Studio" in desc) and "Live2D" in desc)):
  matches.append(i); print("[STUDIO] id={} state={} name={} description={}".format(i,info(o).get("state","?"),n,desc))
for o in objects:
 if o.get("type")!="PipeWire:Interface:Link": continue
 li=info(o); src=li.get("output-node-id"); dst=li.get("input-node-id")
 if src in matches: print("  link {}: {} -> {} ({})".format(o["id"],name(src),name(dst),li.get("state","?")))
if not matches: print("No studio video source found. Keep --broadcast running.")
sys.exit(0 if matches else 1)
'
