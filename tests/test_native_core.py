"""Native launcher integration test; Python is used only as test harness/fake viewer."""
import asyncio
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from websockets.asyncio.client import connect

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'api-rust/target/release/studio'

class NativeCore(unittest.TestCase):
    def setUp(self):
        self.assertTrue(BIN.is_file(), 'Run scripts/build-core.sh first')
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name); self.src = self.base/'original'; self.src.mkdir()
        self.model = self.src/'Test.model3.json'
        self.model.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'body.moc3','Textures':['tex.png'],'Physics':'body.physics3.json'}}))
        (self.src/'body.moc3').touch(); (self.src/'tex.png').touch()
        (self.src/'body.physics3.json').write_text(json.dumps({'Version':3,'Meta':{'Fps':30},'PhysicsSettings':[{'Id':'A','Output':[{'Type':'Angle','Scale':2},{'Type':'X','Scale':7}]}]}))
        (self.src/'Idle.motion3.json').write_text('{"Version":3}')
        (self.src/'Test.vtube.json').write_text(json.dumps({'Version':1,'FileReferences':{'Model':self.model.name,'IdleAnimation':'Idle.motion3.json'},'PhysicsSettings':{'Use':True,'PhysicsStrength':50},'PhysicsCustomizationSettings':{'PhysicsMultipliersPerPhysicsGroup':[{'ID':'A','Value':1.2}]},'ParameterSettings':[{'Input':'MouthPucker','OutputLive2D':'ParamMouthPucker','InputRangeLower':0,'InputRangeUpper':-.15,'OutputRangeLower':0,'OutputRangeUpper':2,'Smoothing':20}]}))
        self.env = dict(os.environ, XDG_DATA_HOME=str(self.base/'data'))
        key = hashlib.sha256(str(self.model.resolve()).encode()).hexdigest()[:24]
        self.profile = self.base/'data/kawari/profiles'/key
        self.before = {p:p.read_bytes() for p in self.src.iterdir()}
    def prepare(self,*args):
        return subprocess.run([str(BIN),'--prepare-only',*args,str(self.model)],env=self.env,capture_output=True,text=True,timeout=15)
    def test_profile_and_reuse(self):
        p=self.prepare(); self.assertEqual(p.returncode,0,p.stderr)
        runtime=self.profile/'runtime'; manifest=json.loads((runtime/self.model.name).read_text())
        self.assertEqual(manifest['FileReferences']['Motions']['Idle'],[{'File':'Idle.motion3.json'}]);self.assertTrue((runtime/'tex.png').is_symlink())
        out=json.loads((runtime/'body.physics3.json').read_text());self.assertAlmostEqual(out['PhysicsSettings'][0]['Output'][0]['Scale'],2.4);self.assertEqual(out['PhysicsSettings'][0]['Output'][1]['Scale'],7)
        rows=(self.profile/'parameters.map').read_text().splitlines(); self.assertIn('MouthPucker ParamMouthPucker 0 -0.15 0 2 100 vts 0 0',rows)
        # Old Python supervisor's lock file can be 0644.
        (self.profile/'session.lock').chmod(0o644)
        p=self.prepare();self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(self.before,{p:p.read_bytes() for p in self.src.iterdir()})
        self.assertNotEqual(self.prepare('--profile-dir',str(self.src/'bad')).returncode,0)
    def test_legacy_data_alias(self):
        legacy=self.base/'data/vtuber-studio';legacy.mkdir(parents=True)
        (legacy/'sentinel').write_text('existing data')
        p=self.prepare();self.assertEqual(p.returncode,0,p.stderr)
        self.assertTrue((self.base/'data/kawari').is_symlink())
        self.assertEqual((legacy/'sentinel').read_text(),'existing data')
        self.assertTrue((legacy/'profiles'/self.profile.name/'parameters.map').is_file())
    def test_plain_model_editable_map(self):
        (self.src/'Test.vtube.json').unlink()
        p=self.prepare();self.assertEqual(p.returncode,0,p.stderr)
        m=self.profile/'parameters.map';self.assertTrue(m.is_file())
        m.write_text('FaceX ParamAngleX -1 1 -30 30 0 vts 1 0\n')
        p=self.prepare();self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(m.read_text(),'FaceX ParamAngleX -1 1 -30 30 0 vts 1 0\n')
    def test_supervisor(self):
        fake=self.base/'fake-viewer'; runtime=self.base/'ipc'
        fake.write_text(f'#!{sys.executable}\n'+'''import os, socket, time
from pathlib import Path
base=Path(os.environ['FAKE_IPC']);base.mkdir(mode=0o700)
sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);sock.bind(str(base/'input.sock'))
(base/'pid').write_text(str(os.getpid()))
assert os.environ['STUDIO_BROADCAST']=='1'
assert os.environ['STUDIO_PARAMETER_INPUT']=='1'
assert os.environ['STUDIO_EDITABLE_MAP']==os.environ['STUDIO_PARAMETER_MAP']
print('[parameters] socket: '+str(base/'input.sock'),flush=True)
while True:
    (base/'parameters.tsv').write_text('P\\tParamMouthPucker\\t0\\t2\\t0\\t0\\nM\\tMouthPucker\\tParamMouthPucker\\t0\\t-0.15\\t0\\t2\\n')
    time.sleep(.05)
''');fake.chmod(0o700)
        env=dict(self.env,STUDIO_VIEWER_BIN=str(fake),FAKE_IPC=str(runtime))
        with socket.socket() as probe: probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        cmd=[str(BIN),'--allow-plugin','VBridger','--port',str(port),'--startup-timeout','5',str(self.model)]
        with (self.base/'log').open('w+') as log:
            child=subprocess.Popen(cmd,env=env,stdout=log,stderr=log)
            try:
                async def check():
                    for _ in range(120):
                        if child.poll() is not None: self.fail((self.base/'log').read_text())
                        try: ws=await connect(f'ws://127.0.0.1:{port}');break
                        except OSError: await asyncio.sleep(.05)
                    else:self.fail('API did not start')
                    async with ws:
                        async def req(kind,data):
                            await ws.send(json.dumps(dict(apiName='VTubeStudioPublicAPI',apiVersion='1.0',messageType=kind,requestID='r',data=data)))
                            return json.loads(await asyncio.wait_for(ws.recv(),3))['data']
                        ident=dict(pluginName='VBridger',pluginDeveloper='Test Dev')
                        token=(await req('AuthenticationTokenRequest',ident))['authenticationToken']
                        self.assertTrue((await req('AuthenticationRequest',dict(ident,authenticationToken=token)))['authenticated'])
                        self.assertEqual(len((await req('Live2DParameterListRequest',{}))['parameters']),1)
                asyncio.run(check())
                self.assertNotEqual(self.prepare().returncode,0,'profile lock must exclude second process')
            finally:
                child.send_signal(signal.SIGTERM)
                try: child.wait(timeout=9)
                except subprocess.TimeoutExpired: child.kill();child.wait();self.fail('supervisor did not stop')
            self.assertEqual(child.returncode,0,(self.base/'log').read_text())
            with self.assertRaises(ProcessLookupError):os.kill(int((runtime/'pid').read_text()),0)
            with socket.socket() as s:s.settimeout(.3);self.assertNotEqual(s.connect_ex(('127.0.0.1',port)),0)

if __name__=='__main__':unittest.main()
