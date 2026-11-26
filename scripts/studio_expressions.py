"""Load local VTS expression metadata and validated Cubism exp3 assets."""
import hashlib
import json
import math
from pathlib import Path
import time


def numeric(v, lo=-1e6, hi=1e6):
    if isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v) or not lo<=v<=hi:
        raise ValueError('invalid expression number')
    return float(v)


class Expressions:
    def __init__(self, model, manifest):
        self.assets={}; self.hotkeys=[]; self.saved=[]; self.active={}; self.warnings=[]
        if model is None: return
        root=model.parent.resolve(); config=model.with_name(model.name.removesuffix('.model3.json')+'.vtube.json')
        try:
            data=json.loads(config.read_text()) if config.is_file() else {}
            if not isinstance(data,dict): raise ValueError('expected object')
        except (OSError,ValueError) as e:
            self.warnings.append(f'{config.name}: {e}');data={}
        hotkeys=data.get('Hotkeys',[])
        hotkeys=[h for h in hotkeys if isinstance(h,dict)] if isinstance(hotkeys,list) else []
        refs=manifest.get('Expressions',[])
        if not isinstance(refs,list): refs=[]
        files={x['File'] for x in refs if isinstance(x,dict) and isinstance(x.get('File'),str)}
        for h in hotkeys:
            if h.get('Action')=='ToggleExpression' and isinstance(h.get('File'),str): files.add(h['File'])
        saved=data.get('SavedActiveExpressions',[])
        self.saved=[x for x in saved if isinstance(x,str)] if isinstance(saved,list) else []
        files.update(self.saved)
        for name in sorted(files):
            if len(self.assets)>=64:
                self.warnings.append('Expression limit (64) reached');break
            try:
                path=(root/name).resolve()
                if not path.is_relative_to(root) or not name.endswith('.exp3.json') or not path.is_file() or path.stat().st_size>1024*1024: raise ValueError('missing or unsafe expression file')
                content=json.loads(path.read_text()); terms=[]; seen=set()
                if content.get('Type','Live2D Expression')!='Live2D Expression': raise ValueError('unsupported type')
                for p in content['Parameters']:
                    n=p['Id']; blend={'Add':'A','Multiply':'M','Overwrite':'O'}[p.get('Blend','Add')]
                    if not isinstance(n,str) or not n or len(n.encode())>128 or any(ord(c)<=32 or ord(c)==127 or c.isspace() for c in n) or n in seen: raise ValueError('invalid or duplicate parameter ID')
                    seen.add(n); terms.append((n,blend,numeric(p['Value'])))
                if not 1<=len(terms)<=256: raise ValueError('unsupported parameter count')
                self.assets[name]=(hashlib.sha256(name.encode()).hexdigest()[:32],terms,numeric(content.get('FadeInTime',.5),0,10))
            except (OSError,ValueError,KeyError,TypeError,AttributeError) as e: self.warnings.append(f'{name}: {e}')
        for h in hotkeys:
            if h.get('Action')!='ToggleExpression' or h.get('File') not in self.assets or not h.get('IsActive',True): continue
            try:
                if h.get('DeactivateAfterKeyUp'): raise ValueError('key-release hotkey not supported')
                fade=numeric(h.get('FadeSecondsAmount',.5),0,10)
                hold=numeric(h.get('DeactivateAfterSecondsAmount',0),0,86400) if h.get('DeactivateAfterSeconds') else 0
                if h.get('DeactivateAfterSeconds') and hold<=0: raise ValueError('invalid timer')
                if not isinstance(h.get('HotkeyID'),str) or not isinstance(h.get('Name'),str): raise ValueError('invalid hotkey identity')
                self.hotkeys.append(dict(id=h['HotkeyID'],name=h['Name'],file=h['File'],fade=fade,hold=hold,screen=h.get('Triggers',{}).get('ScreenButton',-1)))
            except (KeyError,ValueError,TypeError,AttributeError) as e: self.warnings.append(f'{h.get("Name")}: {e}')
    def is_active(self,name):
        return name in self.active and self.active[name]>time.monotonic()
    def activate(self,name,active,fade,hold,params,send):
        if name not in self.assets: raise ValueError('Unknown or unavailable expression')
        key,terms,default=self.assets[name]
        if active:
            missing=[n for n,_,_ in terms if n not in params]
            if missing: raise ValueError('Missing rig parameters: '+', '.join(missing))
            fade=default if fade is None else numeric(fade,0,10)
            packet=f'expr on {key} {round(fade*1000)} {round(hold*1000)} '+' '.join(f'{n} {b} {v:.9g}' for n,b,v in terms)
            if len(packet.encode())>16384: raise ValueError('Expression exceeds IPC limit')
            send(packet); self.active[name]=time.monotonic()+hold if hold else math.inf
        else:
            send('expr off '+key);self.active.pop(name,None)
    def state(self,name,details):
        if name and name not in self.assets: raise ValueError('Unknown or unavailable expression')
        result=[]
        for f,(_,terms,_) in self.assets.items():
            if name and f!=name: continue
            active=self.is_active(f); deadline=self.active.get(f,0); timed=active and math.isfinite(deadline)
            result.append(dict(name=f.removesuffix('.exp3.json'),file=f,active=active,deactivateWhenKeyIsLetGo=False,autoDeactivateAfterSeconds=timed,secondsRemaining=max(0,deadline-time.monotonic()) if timed else 0,usedInHotkeys=[dict(name=h['name'],id=h['id']) for h in self.hotkeys if h['file']==f] if details else [],parameters=[dict(name=n,value=v) for n,_,v in terms] if details else []))
        return result
