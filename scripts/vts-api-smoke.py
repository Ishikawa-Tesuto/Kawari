#!/usr/bin/env python3
"""Send a test motion through the real WebSocket API (not direct IPC)."""
import argparse, asyncio, json, math, time, uuid
from websockets.asyncio.client import connect

async def main(args):
    async with connect(f'ws://127.0.0.1:{args.port}') as ws:
        async def ask(kind,data=None):
            rid=uuid.uuid4().hex
            await ws.send(json.dumps(dict(apiName='VTubeStudioPublicAPI',apiVersion='1.0',requestID=rid,messageType=kind,data=data or {})))
            r=json.loads(await ws.recv())
            if r.get('requestID')!=rid or r.get('messageType')=='APIError': raise RuntimeError(str(r))
            return r['data']
        identity=dict(pluginName='Studio Test',pluginDeveloper='Local Developer')
        token=(await ask('AuthenticationTokenRequest',identity))['authenticationToken']
        if not (await ask('AuthenticationRequest',dict(identity,authenticationToken=token)))['authenticated']: raise RuntimeError('Authentication failed')
        model=await ask('Live2DParameterListRequest')
        print(f"{model['modelName']}: {len(model['parameters'])} Live2D parameters",flush=True)
        start=time.monotonic()
        while (t:=time.monotonic()-start)<args.seconds:
            await ask('InjectParameterDataRequest',dict(mode='set',faceFound=True,parameterValues=[dict(id='FaceAngleX',value=20*math.sin(t*math.tau/4)),dict(id='MouthOpen',value=.5+.5*math.sin(t*math.tau))]))
            await asyncio.sleep(1/60)
        print('Finished. Native inputs expire after one second.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--port',type=int,default=8001); p.add_argument('--seconds',type=float,default=15); a=p.parse_args()
    try: asyncio.run(main(a))
    except KeyboardInterrupt: pass
