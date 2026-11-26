#!/usr/bin/env python3
"""Experimental VTube Studio 1.0 compatibility bridge over private parameter IPC."""
import argparse
import asyncio
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import socket
import stat
import time
import sys
import uuid
from websockets.asyncio.server import serve

sys.path.insert(0,str(Path(__file__).resolve().parent))
from studio_expressions import Expressions
from studio_model_state import ModelState
from studio_tokens import Tokens

API = 'VTubeStudioPublicAPI'
# Input values use VTS units (degrees for angles), not normalized angles.
# Only a subset is exposed here; additional inputs may be created by plugins.
BUILTINS = {
    'FaceAngleX':(-30,30,0,'ParamAngleX'), 'FaceAngleY':(-30,30,0,'ParamAngleY'),
    'FaceAngleZ':(-30,30,0,'ParamAngleZ'), 'EyeOpenLeft':(0,1,1,'ParamEyeLOpen'),
    'EyeOpenRight':(0,1,1,'ParamEyeROpen'), 'EyeLeftX':(-1,1,0,'ParamEyeBallX'),
    'EyeLeftY':(-1,1,0,'ParamEyeBallY'), 'EyeRightX':(-1,1,0,None),
    'EyeRightY':(-1,1,0,None), 'MouthOpen':(0,1,0,'ParamMouthOpenY'),
    'MouthSmile':(-1,1,0,'ParamMouthForm'), 'BrowLeftY':(-1,1,0,'ParamBrowLY'),
    'BrowRightY':(-1,1,0,'ParamBrowRY'), 'FacePositionX':(-10,10,0,None),
    'FacePositionY':(-10,10,0,None), 'FacePositionZ':(-10,10,0,None),
    'CheekPuff':(0,1,0,'ParamCheek'), 'TongueOut':(0,1,0,None),
    'MouthX':(-1,1,0,None), 'Brows':(-1,1,0,None),
    'MousePositionX':(-1,1,0,None), 'MousePositionY':(-1,1,0,None),
    'VoiceVolume':(0,1,0,None), 'VoiceFrequency':(0,1,0,None),
    'VoiceVolumePlusMouthOpen':(0,1,0,None), 'VoiceFrequencyPlusMouthSmile':(0,1,0,None),
    **{n:(0,1,0,None) for n in ('VoiceA','VoiceI','VoiceU','VoiceE','VoiceO','VoiceSilence','FaceAngry')},
}

def envelope(kind, data, request_id=None):
    return dict(apiName=API, apiVersion='1.0', timestamp=int(time.time()*1000),
                requestID=request_id or uuid.uuid4().hex, messageType=kind, data=data)

class APIError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


def number(value):
    if isinstance(value,bool) or not isinstance(value,(int,float)) or abs(value)>1e6 or not math.isfinite(value):
        raise APIError(400,'Invalid numeric value')
    return float(value)


def param(name, minimum, maximum, default, value=None, owner=None):
    p=dict(name=name,min=minimum,max=maximum,defaultValue=default,value=default if value is None else value)
    if owner is not None: p['addedBy']=owner
    return p

class Snapshot:
    def __init__(self,path):
        self.path=Path(path); self.params={}; self.maps={}; self.mtime=None
    def refresh(self):
        try:
            st=self.path.stat()
            if time.time()-st.st_mtime>3: raise OSError('snapshot stale')
            if st.st_mtime_ns==self.mtime: return
            rows=self.path.read_text().splitlines(); params={}; maps={}
            for line in rows:
                parts=line.split('\t')
                if parts[0]=='P' and len(parts)==6:
                    n=parts[1]; lo,hi,initial,value=map(float,parts[2:]); params[n]=param(n,lo,hi,initial,value)
                elif parts[0]=='M' and len(parts)==7:
                    name,target=parts[1:3]; lo,hi,a,b=map(float,parts[3:]); maps.setdefault(name,[]).append((target,lo,hi,a,b))
            if not params: raise OSError('empty snapshot')
            self.params,self.maps,self.mtime=params,maps,st.st_mtime_ns
        except (OSError,ValueError):
            self.params,self.maps,self.mtime={},{},None

class Bridge:
    def __init__(self, sock, model, allowed):
        self.sock=Path(sock); self.snapshot=Snapshot(self.sock.parent/'parameters.tsv')
        self.model=Path(model).resolve() if model else None
        self.allowed={s.casefold() for s in allowed}; self.tokens={}; self.custom={}; self.owners={}; self.route_owners={}; self.values={}; self.face={}
        self.sessions=set(); self.instance=uuid.uuid4().hex
        self.started=time.monotonic(); self.sender=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); self.sender.setblocking(False)
        self.modelname=self.model.name.removesuffix('.model3.json') if self.model else 'Haru'
        self.modelid=hashlib.sha256(str(self.model or 'sample:Haru').encode()).hexdigest()[:32]
        self.manifest={}
        if self.model: self.manifest=json.loads(self.model.read_text()).get('FileReferences',{})
        self.expressions=Expressions(self.model,self.manifest)
        self.state=None
        self.token_store=None
        self.restoring=False
        for warning in self.expressions.warnings: print('[expressions] '+warning,flush=True)
        print(f'[expressions] {len(self.expressions.assets)} files / {len(self.expressions.hotkeys)} supported hotkeys',flush=True)
    def expression_packet(self,packet):
        try: self.sender.sendto(packet.encode(),str(self.sock))
        except OSError as e: raise APIError(400,'Expression IPC unavailable: '+str(e)) from e
    def expression_activate(self,name,active,fade=None,hold=0):
        try: self.expressions.activate(name,active,fade,hold,self.snapshot.params,self.expression_packet)
        except ValueError as e: raise APIError(400,str(e)) from e
        if self.state and not self.restoring:
            self.save_expression_state()
    def save_expression_state(self):
        if not self.state: return
        try: self.state.save([n for n, deadline in self.expressions.active.items() if deadline == math.inf])
        except OSError as e: print(f"[state] save failed: {e}",flush=True)
    def identity(self,data):
        name,dev=data.get('pluginName'),data.get('pluginDeveloper')
        if not all(isinstance(x,str) and 3<=len(x)<=32 for x in (name,dev)):
            raise APIError(50,'Invalid plugin identity')
        return name,dev
    def model_info(self):
        self.snapshot.refresh(); loaded=bool(self.snapshot.params)
        return dict(modelLoaded=loaded, modelName=self.modelname if loaded else '',modelID=self.modelid if loaded else '')
    def inputs(self):
        result={n:param(n,a,b,c,owner='VTube Studio') for n,(a,b,c,_) in BUILTINS.items()}
        for n,items in self.snapshot.maps.items():
            # Calibration endpoints may be reversed and several targets may use one input.
            lo=min(min(a,b) for _,a,b,_,_ in items)
            hi=max(max(a,b) for _,a,b,_,_ in items)
            if n not in result:
                result[n]=param(n,lo,hi,min(hi,max(lo,0)),owner='Studio')
            elif any(x[0]!=n for x in items):
                # Do not shrink the built-in domain to a narrow calibration interval.
                p=result[n]; p.update(min=min(lo,p['min']),max=max(hi,p['max']))
        result.update({n:dict(p) for n,(_,p) in self.custom.items()})
        now=time.monotonic()
        for n,(v,expiry,_) in self.values.items():
            if n in result and expiry>now: result[n]['value']=v
        return result
    def route(self,name):
        if name in self.snapshot.maps: return name
        target=BUILTINS.get(name,(0,0,0,None))[3]
        # Do not override an explicitly configured mapping on the target.
        if target in self.snapshot.maps and any(x[0]==target for x in self.snapshot.maps[target]): return target
        return None
    def send(self, pairs):
        if not pairs: return
        packet='set plugin 1000 '+' '.join(f'{k} {v:.9g}' for k,v in pairs.items())
        if len(packet.encode())>16384 or len(pairs)>256: raise APIError(400,'Too many mapped inputs')
        try: self.sender.sendto(packet.encode(),str(self.sock))
        except OSError as exc: raise APIError(400,'Parameter IPC unavailable: '+str(exc)) from exc
    def request(self, message, session):
        rid=message.get('requestID') if isinstance(message,dict) else None
        if not isinstance(rid,str) or not 1<=len(rid)<=64: rid=uuid.uuid4().hex
        try:
            if not isinstance(message,dict) or message.get('apiName')!=API or message.get('apiVersion')!='1.0': raise APIError(3,'Unsupported API envelope')
            kind=message.get('messageType'); data=message.get('data') or {}
            if not isinstance(kind,str) or not isinstance(data,dict): raise APIError(400,'Invalid request')
            result=self.dispatch(kind,data,session)
            return envelope(kind.removesuffix('Request')+'Response',result,rid)
        except APIError as e: return envelope('APIError',dict(errorID=e.code,message=e.message),rid)
    def dispatch(self,kind,d,s):
        if kind=='APIStateRequest': return dict(active=True,vTubeStudioVersion='0.3.0',currentSessionAuthenticated=s.get('identity') is not None)
        if kind=='AuthenticationTokenRequest':
            identity=self.identity(d)
            if identity[0].casefold() not in self.allowed:
                print(f'[vts-api] Denied plugin {identity!r}',flush=True); raise APIError(50,'Plugin not in explicit --allow-plugin list')
            if identity not in self.tokens and len(self.tokens)>=64: raise APIError(400,'Token limit reached')
            token=self.tokens.get(identity)
            if token is None:
                token=secrets.token_hex(32)
                proposed=dict(self.tokens); proposed[identity]=token
                if self.token_store:
                    try: self.token_store.save(proposed)
                    except OSError as e: raise APIError(400,'Could not persist plugin token') from e
                self.tokens=proposed
            print(f'[vts-api] Allowed plugin {identity!r} (explicit CLI allowlist)',flush=True)
            return dict(authenticationToken=token)
        if kind=='AuthenticationRequest':
            identity=self.identity(d); token=d.get('authenticationToken'); expected=self.tokens.get(identity)
            ok=identity[0].casefold() in self.allowed and expected is not None and isinstance(token,str) and token.isascii() and secrets.compare_digest(expected,token)
            s['identity']=identity if ok else None
            return dict(authenticated=ok,reason='Token valid' if ok else 'Invalid token; request a new token')
        if s.get('identity') is None: raise APIError(8,'Request requires authentication')
        info=self.model_info(); inputs=self.inputs(); identity=s['identity']; now=time.monotonic()
        if kind=='CurrentModelRequest':
            return dict(**info,vtsModelName='',vtsModelIconName='',live2DModelName=(self.modelname+'.model3.json') if info['modelLoaded'] else '',numberOfLive2DParameters=len(self.snapshot.params),hasPhysicsFile=bool(self.manifest.get('Physics')),numberOfTextures=len(self.manifest.get('Textures',[])))
        if kind=='Live2DParameterListRequest': return dict(**info,parameters=list(self.snapshot.params.values()))
        if kind=='InputParameterListRequest': return dict(**info,defaultParameters=[p for n,p in inputs.items() if n not in self.custom],customParameters=[inputs[n] for n in self.custom])
        if kind=='ParameterValueRequest':
            n=d.get('name');
            if not isinstance(n,str) or n not in inputs: raise APIError(453,'Parameter not found')
            return inputs[n]
        if kind=='HotkeysInCurrentModelRequest':
            if d.get('live2DItemFileName') or d.get('modelID') not in (None,'',self.modelid): raise APIError(400,'Only current model supported')
            return dict(**info,availableHotkeys=[dict(name=h['name'],type='ToggleExpression',description='Toggle expression',file=h['file'],hotkeyID=h['id'],keyCombination=[],onScreenButtonID=h['screen']) for h in self.expressions.hotkeys] if info['modelLoaded'] else [])
        if kind=='ExpressionStateRequest':
            name=d.get('expressionFile','')
            if not isinstance(name,str): raise APIError(400,'Invalid expression file')
            try: result=self.expressions.state(name,d.get('details') is True) if info['modelLoaded'] else []
            except ValueError as e: raise APIError(400,str(e)) from e
            return dict(**info,expressions=result)
        if kind=='ExpressionActivationRequest':
            if not info['modelLoaded'] or not isinstance(d.get('expressionFile'),str) or not isinstance(d.get('active'),bool): raise APIError(400,'Invalid expression request or model unavailable')
            self.expression_activate(d['expressionFile'],d['active'],max(0,min(2,number(d.get('fadeTime',.25)))))
            return {}
        if kind=='HotkeyTriggerRequest':
            key=d.get('hotkeyID')
            if not info['modelLoaded'] or not isinstance(key,str) or d.get('itemInstanceID'): raise APIError(400,'Invalid hotkey request or model unavailable')
            h=next((h for h in self.expressions.hotkeys if h['id']==key or h['name'].casefold()==key.casefold()),None)
            if h is None: raise APIError(400,'Unsupported or unknown hotkey')
            self.expression_activate(h['file'],not self.expressions.is_active(h['file']),h['fade'],h['hold'])
            return dict(hotkeyID=h['id'])
        if kind=='FaceFoundRequest': return dict(found=any(expiry>now for expiry in self.face.values()))
        if kind=='ParameterCreationRequest':
            n=d.get('parameterName'); a,b,c=[number(d.get(k)) for k in ('min','max','defaultValue')]
            if not isinstance(n,str) or not re.fullmatch(r'[A-Za-z0-9]{4,32}',n) or a>=b or not a<=c<=b: raise APIError(400,'Invalid custom parameter definition')
            if n in BUILTINS or (n in self.custom and self.custom[n][0]!=identity): raise APIError(400,'Parameter already owned')
            if n not in self.custom and (len(self.custom)>=300 or sum(o==identity for o,_ in self.custom.values())>=100): raise APIError(400,'Custom parameter limit reached')
            self.custom[n]=(identity,param(n,a,b,c,owner=identity[0])); return dict(parameterName=n)
        if kind=='ParameterDeletionRequest':
            n=d.get('parameterName');
            if not isinstance(n,str) or n not in self.custom or self.custom[n][0]!=identity: raise APIError(400,'Parameter not owned by this plugin')
            del self.custom[n]; self.values.pop(n,None); self.owners.pop(n,None); return dict(parameterName=n)
        if kind=='InjectParameterDataRequest':
            if not info['modelLoaded']: raise APIError(400,'Model IPC not ready')
            if d.get('mode','set')!='set': raise APIError(400,'Only set mode is supported in this prototype')
            values=d.get('parameterValues');
            if not isinstance(values,list) or not 1<=len(values)<=300: raise APIError(400,'Invalid parameterValues')
            pending={}; routes={}
            for item in values:
                if not isinstance(item,dict): raise APIError(400,'Invalid parameter entry')
                n=item.get('id'); v=number(item.get('value')); weight=number(item.get('weight',1))
                if not isinstance(n,str) or n not in inputs or n in pending: raise APIError(453,'Unknown or duplicate input parameter')
                if weight!=1: raise APIError(400,'Only weight=1 is supported in this prototype')
                own=self.owners.get(n)
                if own and own[1]>now and own[0]!=s['id']: raise APIError(400,'Parameter controlled by another session')
                pending[n]=v; route=self.route(n)
                if route is not None:
                    if route in routes: raise APIError(400,'Conflicting inputs for one model mapping')
                    ro=self.route_owners.get(route)
                    if ro and ro[1]>now and ro[0]!=s['id']: raise APIError(400,'Model mapping controlled by another session')
                    routes[route]=v
            self.send(routes)
            for route in routes: self.route_owners[route]=(s['id'],now+1)
            for n,v in pending.items(): self.values[n]=(v,now+1,s['id']); self.owners[n]=(s['id'],now+1)
            if d.get('faceFound') is True: self.face[s['id']]=now+1
            elif d.get('faceFound') is False: self.face.pop(s['id'],None)
            return {}
        raise APIError(7,'Unsupported request: '+kind)
    async def handle(self,ws):
        if len(self.sessions)>=16:
            await ws.close(code=1013,reason='Connection limit reached'); return
        s={'id':uuid.uuid4().hex,'identity':None}; self.sessions.add(s['id'])
        try:
            async for raw in ws:
                try: msg=json.loads(raw)
                except (ValueError,TypeError): msg=None
                reply=self.request(msg,s)
                await ws.send(json.dumps(reply,allow_nan=False,separators=(',',':')))
        finally:
            self.sessions.discard(s['id']); self.face.pop(s['id'],None)
            # Native values expire after 1 s; never clear other clients' data.
    async def discovery(self,port):
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
            while self.sock.exists():
                data=dict(active=True,port=port,instanceID=self.instance,windowTitle='Kawari')
                try: udp.sendto(json.dumps(envelope('VTubeStudioAPIStateBroadcast',data,'VTubeStudioAPIStateBroadcast')).encode(),('127.0.0.1',47779))
                except OSError: pass
                await asyncio.sleep(2)

async def run(args):
    path=Path(args.socket)
    st=path.stat()
    if not stat.S_ISSOCK(st.st_mode) or st.st_uid!=os.getuid() or path.parent.stat().st_uid!=os.getuid() or path.parent.stat().st_mode & 0o077:
        raise SystemExit('Expected private, user-owned P0.3a parameter socket')
    bridge=Bridge(path,args.model,args.allow_plugin)
    try:
        if getattr(args,"token_file",None):
            if bridge.model and Path(args.token_file).resolve().is_relative_to(bridge.model.parent):
                raise ValueError('Token file must be outside original model directory')
            bridge.token_store=Tokens(args.token_file,bridge.allowed)
            bridge.tokens=dict(bridge.token_store.values)
            # Persist only currently allowed entries, including revocation by removing a name.
            if Path(args.token_file).exists(): bridge.token_store.save(bridge.tokens)
        if args.state_file:
            bridge.state=ModelState(args.state_file,bridge.modelid,bridge.model)
        async with serve(bridge.handle,'127.0.0.1',args.port,origins=[None],max_size=1024*1024,max_queue=16):
            if bridge.expressions.assets:
                bridge.snapshot.refresh()
                if not bridge.snapshot.params: raise ValueError('Wait until model parameter snapshot is ready')
                bridge.expression_packet('expr reset')
                saved=bridge.state.saved if bridge.state and bridge.state.saved is not None else (bridge.expressions.saved if args.restore_expressions else [])
                bridge.restoring=True
                try:
                    for name in saved:
                        try: bridge.expression_activate(name,True)
                        except APIError as e: print(f'[expressions] skipped saved {name}: {e.message}',flush=True)
                finally: bridge.restoring=False
                if bridge.state and bridge.state.saved is None: bridge.save_expression_state()
            print(f'[vts-api] Listening on ws://127.0.0.1:{args.port}; model {bridge.modelname}; allowed {args.allow_plugin}',flush=True)
            await bridge.discovery(args.port)
    finally: bridge.sender.close()

def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--socket',required=True); p.add_argument('--model',type=Path); p.add_argument('--port',type=int,default=8001); p.add_argument('--allow-plugin',action='append',required=True)
    p.add_argument("--restore-expressions",action="store_true",help="activate SavedActiveExpressions at startup")
    p.add_argument("--state-file",type=Path,help="persist untimed expression toggles outside original model folder; restore them on startup")
    p.add_argument("--token-file",type=Path,help="persist authorized plugin tokens in a private file")
    args=p.parse_args()
    if not 1<=args.port<=65535: p.error('port must be 1..65535')
    try: asyncio.run(run(args))
    except KeyboardInterrupt: pass
    except APIError as e: raise SystemExit(f"[vts-api] {e.message}")
    except (OSError,ValueError) as e: raise SystemExit(f'[vts-api] {e}')

if __name__=='__main__': main()
