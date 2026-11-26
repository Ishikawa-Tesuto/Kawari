import asyncio
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from websockets.asyncio.client import connect

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import studio
from studio_tokens import Tokens

FAKE = '''import os, socket, sys, time
from pathlib import Path
base=Path(sys.argv[1]); base.mkdir(mode=0o700)
sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); sock.bind(str(base/'input.sock'))
(base/'pid').write_text(str(os.getpid()))
print('[parameters] socket: '+str(base/'input.sock'),flush=True)
while True:
    (base/'parameters.tsv').write_text('P\\tParamAngleX\\t-30\\t30\\t0\\t0\\nM\\tParamAngleX\\tParamAngleX\\t-30\\t30\\t-30\\t30\\n')
    time.sleep(.05)
'''

class Unified(unittest.TestCase):
    def test_token_store(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'tokens.json'; identity=('VBridger','Test Dev')
            t=Tokens(path,{'vbridger'}); t.save({identity:'a'*64,('Other','Test Dev'):'b'*64})
            self.assertEqual(path.stat().st_mode & 0o777,0o600)
            self.assertEqual(Tokens(path,{'vbridger'}).values,{identity:'a'*64})
            self.assertEqual(Tokens(path,set()).values,{})
            path.chmod(0o644)
            with self.assertRaises(ValueError): Tokens(path,{'vbridger'})
            path.unlink(); path.symlink_to(Path(td)/'missing')
            with self.assertRaises(OSError): Tokens(path,{'vbridger'})

    def test_real_websocket_restart(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td); tokens=base/'tokens.json'; fake=base/'viewer.py'; fake.write_text(FAKE)
            identity=dict(pluginName='VBridger',pluginDeveloper='Test Dev')
            async def auth(port, old):
                async with connect(f'ws://127.0.0.1:{port}') as ws:
                    async def req(kind,data):
                        await ws.send(json.dumps(dict(apiName='VTubeStudioPublicAPI',apiVersion='1.0',messageType=kind,data=data,requestID='r')))
                        return json.loads(await ws.recv())['data']
                    if old is None: old=(await req('AuthenticationTokenRequest',identity))['authenticationToken']
                    self.assertTrue((await req('AuthenticationRequest',dict(identity,authenticationToken=old)))['authenticated'])
                    self.assertEqual(len((await req('Live2DParameterListRequest',{}))['parameters']),1)
                    return old
            token=None
            for i in range(2):
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]
                stopping=threading.Event(); result=[]; errors=[]; runtime=base/f'run{i}'
                def worker():
                    try: result.append(studio.supervise([sys.executable,str(fake),str(runtime)],[sys.executable,'-u',str(ROOT/'scripts/vts-api.py'),'--allow-plugin','VBridger','--port',str(port),'--token-file',str(tokens)],stopping,5))
                    except Exception as e: errors.append(e)
                thread=threading.Thread(target=worker); thread.start()
                try:
                    deadline=time.monotonic()+5
                    while True:
                        try: token=asyncio.run(auth(port,token)); break
                        except OSError:
                            if time.monotonic()>deadline or errors: raise
                            time.sleep(.05)
                finally:
                    stopping.set(); thread.join(8)
                self.assertFalse(thread.is_alive()); self.assertFalse(errors); self.assertEqual(result,[0])
                with self.assertRaises(ProcessLookupError): os.kill(int((runtime/'pid').read_text()),0)
                with socket.socket() as check:
                    check.settimeout(.3); self.assertNotEqual(check.connect_ex(('127.0.0.1',port)),0)

    def test_viewer_failure(self):
        with self.assertRaisesRegex(RuntimeError,'Viewer exited'):
            studio.supervise([sys.executable,'-c','raise SystemExit(7)'],['unused'],threading.Event(),2)

    def test_api_failure_stops_viewer(self):
        with tempfile.TemporaryDirectory() as td:
            base=Path(td); fake=base/'viewer.py'; fake.write_text(FAKE); runtime=base/'run'
            with self.assertRaisesRegex(RuntimeError,'API exited'):
                studio.supervise([sys.executable,str(fake),str(runtime)],[sys.executable,'-c','raise SystemExit(8)'],threading.Event(),3)
            with self.assertRaises(ProcessLookupError): os.kill(int((runtime/'pid').read_text()),0)

    def test_prepare_and_reuse_profile(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); src=root/'original'; src.mkdir()
            model=src/'Test.model3.json'; model.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'body.moc3','Textures':['tex.png']}}))
            (src/'body.moc3').touch(); (src/'tex.png').touch()
            cfg=src/'Test.vtube.json'; cfg.write_text(json.dumps({'Version':1,'FileReferences':{'Model':model.name},'ParameterSettings':[dict(Input='FaceAngleX',OutputLive2D='ParamAngleX',InputRangeLower=-30,InputRangeUpper=30,OutputRangeLower=-30,OutputRangeUpper=30,Smoothing=0)]}))
            before={p:p.read_bytes() for p in src.iterdir()}
            args=SimpleNamespace(model=model,profile_dir=root/'profile',runtime_model=None,parameter_map=None,state_file=None)
            first=studio.prepare(args)
            try:
                self.assertTrue(first[1].exists()); self.assertTrue(first[2].exists())
                with self.assertRaises(BlockingIOError): studio.prepare(args)
            finally: first[-1].close()
            second=studio.prepare(args)
            try: self.assertEqual(first[:-1],second[:-1])
            finally: second[-1].close()
            self.assertEqual(before,{p:p.read_bytes() for p in src.iterdir()})
