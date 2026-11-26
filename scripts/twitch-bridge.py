#!/usr/bin/env python3
"""Optional Twitch Channel Points integration. No Twitch credentials enter Kawari IPC."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import stat
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import OrderedDict
from websockets.sync.client import connect
from websockets.exceptions import ConnectionClosed

SCOPE = 'channel:read:redemptions'
IDENTITY = {'pluginName': 'Kawari Twitch', 'pluginDeveloper': 'Kawari'}
EVENT = 'channel.channel_points_custom_reward_redemption.add'
OAUTH = 'https://id.twitch.tv/oauth2/'
HELIX = 'https://api.twitch.tv/helix/'
WS = 'wss://eventsub.wss.twitch.tv/ws'


def directory():
    return Path(os.environ.get('XDG_DATA_HOME') or Path.home()/'.local/share')/'kawari'/'twitch'


def private_read(path, default=None):
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return default
    with os.fdopen(fd) as f:
        st = os.fstat(f.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077 or st.st_size > 1024*1024:
            raise RuntimeError(f'Expected private file: {path}')
        return json.load(f)


def private_write(path, value):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as f:
        name = f.name
        try:
            json.dump(value, f, indent=2); f.flush(); os.fsync(f.fileno())
            os.replace(name, path)
        finally:
            if os.path.exists(name): os.unlink(name)


class HttpError(RuntimeError):
    def __init__(self, status, message):
        super().__init__(f'Twitch HTTP {status}: {message}')
        self.status = status
        self.message = message


def http(url, *, form=None, body=None, headers=None):
    headers = dict(headers or {})
    data = None
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
    if body is not None:
        data = json.dumps(body).encode(); headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read(1024*1024))
    except urllib.error.HTTPError as e:
        # Never print HTTP bodies, headers, tokens, device codes or refresh tokens.
        message = ''
        try:
            candidate = json.loads(e.read(8192)).get('message', '')
            if candidate in ('authorization_pending', 'slow_down', 'invalid device code', 'Invalid refresh token'):
                message = candidate
        except (ValueError, AttributeError): pass
        raise HttpError(e.code, message) from None


def login(client, path):
    d = http(OAUTH+'device', form={'client_id': client, 'scopes': SCOPE})
    # Only the user code and official verification URL are displayed.
    url = d['verification_uri']; parsed = urllib.parse.urlparse(url)
    if parsed.scheme != 'https' or parsed.hostname not in ('www.twitch.tv', 'twitch.tv', 'id.twitch.tv'):
        raise RuntimeError('Unexpected verification URL')
    print(f'Open {url}\nCode: {d["user_code"]}', flush=True)
    delay = max(5, int(d.get('interval', 5)))
    end = time.monotonic()+int(d['expires_in'])
    while time.monotonic() < end:
        time.sleep(delay)
        try:
            token = http(OAUTH+'token', form={'client_id':client,'scopes':SCOPE,'device_code':d['device_code'],'grant_type':'urn:ietf:params:oauth:grant-type:device_code'})
            token['client_id']=client; private_write(path,token); return token
        except HttpError as e:
            if e.message == 'authorization_pending': continue
            if e.message == 'slow_down': delay+=5; continue
            raise
    raise RuntimeError('Device authorization expired')


def authorized(client, path, interactive=False):
    token = private_read(path)
    if not token or token.get('client_id') != client:
        if not interactive: raise RuntimeError('Run login first')
        token = login(client,path)
    def validate(t):
        v = http(OAUTH+'validate',headers={'Authorization':'OAuth '+t['access_token']})
        if v.get('client_id') != client or SCOPE not in v.get('scopes',[]) or not v.get('user_id'):
            raise RuntimeError('Wrong Twitch client or missing redemption scope; log in again')
        return v
    try: v=validate(token)
    except HttpError as e:
        if e.status != 401: raise
        token=refresh(client,path,token);v=validate(token)
    if v.get('expires_in',0)<120:
        token=refresh(client,path,token);v=validate(token)
    return token,v


def refresh(client,path,token):
    try:
        t=http(OAUTH+'token',form={'client_id':client,'grant_type':'refresh_token','refresh_token':token['refresh_token']})
    except (KeyError,HttpError): raise RuntimeError('Twitch session expired; run login again') from None
    t['client_id']=client;private_write(path,t);return t


def helix(client, token, endpoint, body=None):
    return http(HELIX+endpoint,body=body,headers={'Authorization':'Bearer '+token['access_token'],'Client-Id':client})


class LocalApi:
    def __init__(self, port, path): self.port=port;self.path=path;self.ws=None;self.n=0
    def __enter__(self):
        self._connection=connect(f'ws://127.0.0.1:{self.port}',open_timeout=5,close_timeout=1)
        self.ws=self._connection.__enter__()
        try:
            token=private_read(self.path,{}).get('token','')
            if not token or not self.call('AuthenticationRequest',dict(IDENTITY,authenticationToken=token))['authenticated']:
                print('Approve Kawari Twitch in Kawari > Integrations > Plugins (60 seconds).',flush=True)
                token=self.call('AuthenticationTokenRequest',IDENTITY,timeout=65)['authenticationToken']
                if not self.call('AuthenticationRequest',dict(IDENTITY,authenticationToken=token))['authenticated']:
                    raise RuntimeError('Kawari authorization failed')
                private_write(self.path,{'token':token})
            return self
        except BaseException:
            self._connection.__exit__(None,None,None); raise
    def __exit__(self,*args): self._connection.__exit__(*args)
    def call(self,kind,data=None,timeout=5):
        self.n+=1; rid=str(self.n)
        self.ws.send(json.dumps({'apiName':'VTubeStudioPublicAPI','apiVersion':'1.0','requestID':rid,'messageType':kind,'data':data or {}}))
        r=json.loads(self.ws.recv(timeout=timeout))
        if r.get('requestID')!=rid or r.get('messageType')!=kind.removesuffix('Request')+'Response':
            raise RuntimeError('Kawari API rejected '+kind+': '+str(r.get('data',{}).get('message','unexpected response')))
        return r['data']


class Redemptions:
    def __init__(self): self.seen=OrderedDict()
    def take(self,message,broadcaster):
        if message.get('metadata',{}).get('message_type')!='notification': return None
        payload=message.get('payload',{}); sub=payload.get('subscription',{}); event=payload.get('event',{})
        if sub.get('type')!=EVENT or event.get('broadcaster_user_id')!=broadcaster: return None
        eid=event.get('id'); reward=event.get('reward',{}).get('id')
        if not isinstance(eid,str) or not eid or not isinstance(reward,str) or not reward:return None
        if eid in self.seen:return None
        self.seen[eid]=None
        if len(self.seen)>4096:self.seen.popitem(last=False)
        return reward


def dispatch(api, bindings, reward):
    model=api.call('CurrentModelRequest')
    if not model.get('modelLoaded'): return False
    row=bindings.get(model['modelID'],{}).get(reward)
    if not row:return False
    api.call('KawariRewardExpressionRequest',dict(row,modelID=model['modelID']))
    return True


def run(client, path, port, bindings_path, local_path):
    dedup=Redemptions(); token,who=authorized(client,path)
    # Approve the local plugin before opening the Twitch subscription.
    with LocalApi(port,local_path): pass
    broadcaster=who['user_id']; next_validate=time.monotonic()+min(1800,max(30,who['expires_in']-120))
    # On an ordinary disconnect resubscribe. Twitch cannot replay missed events.
    while True:
        old=None;ws=None
        try:
            token,who=authorized(client,path)
            if who['user_id']!=broadcaster: raise RuntimeError('Twitch account changed; restart bridge')
            ws=connect(WS,open_timeout=10,close_timeout=1,ping_interval=None)
            welcome=json.loads(ws.recv(timeout=10));session=welcome['payload']['session']
            if welcome['metadata']['message_type']!='session_welcome':raise RuntimeError('Expected EventSub welcome')
            helix(client,token,'eventsub/subscriptions',{'type':EVENT,'version':'1','condition':{'broadcaster_user_id':broadcaster},'transport':{'method':'websocket','session_id':session['id']}})
            print('Twitch connected: '+who['login'],flush=True)
            with LocalApi(port,local_path) as api:
                while True:
                    if time.monotonic()>=next_validate:
                        token,who=authorized(client,path)
                        if who['user_id']!=broadcaster:raise RuntimeError('Twitch account changed; restart bridge')
                        next_validate=time.monotonic()+min(1800,max(30,who['expires_in']-120))
                    message=json.loads(ws.recv(timeout=int(session.get('keepalive_timeout_seconds') or 30)+10))
                    kind=message['metadata']['message_type']
                    if kind=='session_reconnect':
                        url=message['payload']['session']['reconnect_url'];u=urllib.parse.urlparse(url)
                        if u.scheme!='wss' or u.hostname!='eventsub.wss.twitch.tv':raise RuntimeError('Unexpected reconnect URL')
                        old=ws;ws=connect(url,open_timeout=10,close_timeout=1,ping_interval=None)
                        w=json.loads(ws.recv(timeout=10))
                        if w['metadata']['message_type']!='session_welcome':raise RuntimeError('Expected reconnect welcome')
                        session=w['payload']['session'];old.close();old=None
                    elif kind=='revocation':raise RuntimeError('Twitch revoked subscription; check account authorization')
                    elif kind=='notification':
                        reward=dedup.take(message,broadcaster)
                        if reward:
                            try:
                                if dispatch(api,private_read(bindings_path,{}),reward):print('Reward applied: '+reward,flush=True)
                            except RuntimeError as e: print(str(e),flush=True)
        except (ConnectionClosed,TimeoutError,OSError):
            print('Connection lost; reconnecting in 5 seconds',flush=True);time.sleep(5)
        finally:
            if old is not None:old.close()
            if ws is not None:ws.close()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--client-id',default=os.environ.get('KAWARI_TWITCH_CLIENT_ID'));p.add_argument('--port',type=int,default=8001)
    sub=p.add_subparsers(dest='action',required=True)
    for a in ('login','rewards','expressions','run','logout','bindings'):sub.add_parser(a)
    b=sub.add_parser('bind');b.add_argument('reward_id');b.add_argument('expression');b.add_argument('--mode',choices=['on','off','toggle','timed'],default='toggle');b.add_argument('--duration',type=float,default=10)
    b=sub.add_parser('unbind');b.add_argument('reward_id')
    args=p.parse_args();root=directory();path=root/'oauth.json';local=root/'kawari-token.json';bindings=root/'bindings.json'
    root.mkdir(mode=0o700,parents=True,exist_ok=True)
    # Serialize the legacy CLI with the optional native process.
    lock_fd=os.open(root/'bridge.lock',os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW|os.O_CLOEXEC,0o600)
    with os.fdopen(lock_fd,'rb') as lock:
        try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError: raise RuntimeError('Native Twitch or another bridge is running; stop it first (KAWARI_TWITCH_NATIVE=0 for legacy CLI)') from None
        execute(args,p,path,local,bindings)

def execute(args,p,path,local,bindings):
    if args.action=='logout':path.unlink(missing_ok=True);print('Local Twitch credentials removed; revoke Twitch app access in account settings if desired.');return
    if args.action=='bindings':print(json.dumps(private_read(bindings,{}),indent=2));return
    if args.action in ('expressions','bind','unbind'):
        with LocalApi(args.port,local) as api:
            model=api.call('CurrentModelRequest');mid=model['modelID']
            if not model['modelLoaded']:raise RuntimeError('No model loaded')
            if args.action=='expressions':print(json.dumps(api.call('ExpressionStateRequest')['expressions'],indent=2,ensure_ascii=False));return
            rows=private_read(bindings,{});m=rows.setdefault(mid,{})
            if args.action=='unbind':m.pop(args.reward_id,None)
            else:
                if not 0<args.duration<=300:raise RuntimeError('Duration must be 0–300 seconds (exclusive zero)')
                # API validates actual expression file at redemption; verify now as well.
                api.call('ExpressionStateRequest',{'expressionFile':args.expression})
                m[args.reward_id]={'expressionFile':args.expression,'mode':args.mode,'duration':args.duration if args.mode=='timed' else 0}
            private_write(bindings,rows);print('Saved for '+model['modelName']);return
    if not args.client_id:p.error('Set --client-id or KAWARI_TWITCH_CLIENT_ID (registered public Twitch application)')
    if args.action=='login':
        login(args.client_id,path);_,who=authorized(args.client_id,path);print('Authorized: '+who['login'])
    elif args.action=='rewards':
        token,who=authorized(args.client_id,path);r=helix(args.client_id,token,'channel_points/custom_rewards?'+urllib.parse.urlencode({'broadcaster_id':who['user_id']}));print(json.dumps([{'id':x['id'],'title':x['title'],'cost':x['cost']} for x in r['data']],indent=2,ensure_ascii=False))
    else:run(args.client_id,path,args.port,bindings,local)


if __name__=='__main__':
    try:main()
    except KeyboardInterrupt:pass
    except (RuntimeError,ValueError,OSError) as e:raise SystemExit(str(e))
