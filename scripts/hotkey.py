#!/usr/bin/env python3
"""Local CLI for VTS-compatible expression and hotkey requests (also KDE shortcuts)."""
import argparse
import asyncio
import json
import uuid
from websockets.asyncio.client import connect

async def run(args):
    async with connect(f'ws://127.0.0.1:{args.port}',open_timeout=3) as ws:
        async def request(kind,data):
            rid=uuid.uuid4().hex
            await ws.send(json.dumps(dict(apiName='VTubeStudioPublicAPI',apiVersion='1.0',requestID=rid,messageType=kind,data=data)))
            r=json.loads(await asyncio.wait_for(ws.recv(),3))
            if r['messageType']=='APIError': raise ValueError(r['data']['message'])
            return r['data']
        identity=dict(pluginName='Studio Hotkeys',pluginDeveloper='Kawari')
        token=(await request('AuthenticationTokenRequest',identity))['authenticationToken']
        if not (await request('AuthenticationRequest',dict(identity,authenticationToken=token)))['authenticated']: raise ValueError('Authentication failed')
        if args.action=='list': data=await request('HotkeysInCurrentModelRequest',{})
        elif args.action=='state': data=await request('ExpressionStateRequest',{'details':True})
        elif args.action=='trigger': data=await request('HotkeyTriggerRequest',{'hotkeyID':args.name})
        else: data=await request('ExpressionActivationRequest',{'expressionFile':args.name,'active':args.action=='on'})
        print(json.dumps(data,ensure_ascii=False,indent=2))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',type=int,default=8001)
    sub=p.add_subparsers(dest='action',required=True)
    for a in ('list','state','trigger','on','off'):
        s=sub.add_parser(a)
        if a in ('trigger','on','off'):s.add_argument('name')
    try: asyncio.run(run(p.parse_args()))
    except (OSError,ValueError,TimeoutError) as e: raise SystemExit(str(e))
if __name__=='__main__':main()
