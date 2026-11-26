"""Executable integration test for both API backends, using fake native IPC only."""
import asyncio
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from websockets.asyncio.client import connect

ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'api-rust/target/release/studio-vts-api'
BACKEND=os.environ.get('STUDIO_API_TEST_BACKEND','rust')

@unittest.skipUnless(BACKEND=='python' or BIN.is_file(), 'Build Rust API first: scripts/build-api.sh')
class NativeApi(unittest.IsolatedAsyncioTestCase):
    async def test_full_protocol_and_restart(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); ipc=root/'ipc'; ipc.mkdir(mode=0o700)
            rx=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); rx.bind(str(ipc/'input.sock')); rx.setblocking(False)
            snap=ipc/'parameters.tsv'; snap.write_text('P\tParamAngleX2\t-30\t30\t0\t0\nP\tHaloToggle\t0\t6\t0\t0\nM\tFaceAngleX\tParamAngleX2\t-40\t40\t-30\t30\n')
            stop=threading.Event()
            def refresh():
                while not stop.wait(.1): os.utime(snap,None)
            thread=threading.Thread(target=refresh); thread.start()
            src=root/'model';src.mkdir();model=src/'Test.model3.json'
            model.write_text(json.dumps({'Version':3,'FileReferences':{'Expressions':[{'Name':'Halo','File':'Halo.exp3.json'}]}}))
            (src/'Halo.exp3.json').write_text(json.dumps({'Type':'Live2D Expression','Parameters':[{'Id':'HaloToggle','Value':1,'Blend':'Overwrite'}]}))
            (src/'Test.vtube.json').write_text(json.dumps({'Hotkeys':[{'Action':'ToggleExpression','Name':'Halo','HotkeyID':'halo','File':'Halo.exp3.json','FadeSecondsAmount':.3,'Triggers':{'Trigger1':'Numpad5','Trigger2':'','Trigger3':''},'IsGlobal':True},{'Action':'ToggleExpression','Name':'Halo Alternate','HotkeyID':'halo-alt','File':'Halo.exp3.json','FadeSecondsAmount':.3,'Triggers':{'Trigger1':'U','Trigger2':'LeftControl','Trigger3':'RightShift'},'IsGlobal':True}]}))
            source_config=(src/'Test.vtube.json').read_bytes()
            tokens=root/'tokens.json'; state=root/'state.json'; token=None
            try:
                for iteration in range(2):
                    with socket.socket() as probe: probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
                    backend=os.environ.get('STUDIO_API_TEST_FIRST',BACKEND) if iteration==0 else BACKEND
                    cmd=([sys.executable,'-u',str(ROOT/'scripts/vts-api.py')] if backend=='python' else [str(BIN)])
                    cmd+=['--socket',str(ipc/'input.sock'),'--model',str(model),'--port',str(port),'--allow-plugin','VBridger','--token-file',str(tokens),'--state-file',str(state),'--restore-expressions']
                    child=subprocess.Popen(cmd,stdout=subprocess.DEVNULL,env=dict(os.environ,KAWARI_GLOBAL_SHORTCUTS="0"))
                    try:
                        for attempt in range(100):
                            if child.poll() is not None: self.fail(f'API exited {child.returncode}')
                            try: ws=await connect(f'ws://127.0.0.1:{port}');break
                            except OSError: await asyncio.sleep(.05)
                        else: self.fail('API did not start')
                        async with ws:
                            async def req(kind,data=None,error=None):
                                await ws.send(json.dumps(dict(apiName='VTubeStudioPublicAPI',apiVersion='1.0',messageType=kind,requestID='test',data=data or {})))
                                r=json.loads(await asyncio.wait_for(ws.recv(),3));self.assertEqual(r['requestID'],'test')
                                if error is not None: self.assertEqual(r['messageType'],'APIError');self.assertEqual(r['data']['errorID'],error)
                                else: self.assertEqual(r['messageType'],kind.removesuffix('Request')+'Response',r)
                                return r['data']
                            async def packet(): return (await asyncio.wait_for(asyncio.get_running_loop().sock_recv(rx,16384),2)).decode()
                            self.assertEqual(await packet(),'expr reset')
                            await req('Live2DParameterListRequest',error=8)
                            await req('AuthenticationTokenRequest',{'pluginName':'Unknown','pluginDeveloper':'Test Dev'},error=50)
                            identity={'pluginName':'VBridger','pluginDeveloper':'Test Dev'}
                            if iteration==0: token=(await req('AuthenticationTokenRequest',identity))['authenticationToken']
                            self.assertTrue((await req('AuthenticationRequest',dict(identity,authenticationToken=token)))['authenticated'])
                            self.assertEqual(tokens.stat().st_mode & 0o777,0o600)
                            self.assertEqual(len((await req('Live2DParameterListRequest'))['parameters']),2)
                            if iteration==0:
                                await req('ParameterCreationRequest',dict(parameterName='MouthCustom',min=-1,max=1,defaultValue=0))
                                await req('InjectParameterDataRequest',dict(faceFound=True,parameterValues=[dict(id='FaceAngleX',value=20),dict(id='MouthCustom',value=.5)]))
                                self.assertEqual(await packet(),'set plugin 1000 FaceAngleX 20')
                                self.assertEqual((await req('ParameterValueRequest',{'name':'MouthCustom'}))['value'],.5)
                                self.assertTrue((await req('FaceFoundRequest'))['found'])
                                await req('InjectParameterDataRequest',dict(parameterValues=[dict(id='FaceAngleX',value=10),dict(id='Missing',value=0)]),error=453)
                                with self.assertRaises(BlockingIOError): rx.recv(16384)
                                await req('HotkeyTriggerRequest',{'hotkeyID':'halo'})
                                self.assertTrue((await packet()).startswith('expr on '))
                            else:
                                self.assertTrue((await packet()).startswith('expr on '))
                            self.assertTrue((await req('ExpressionStateRequest',{'details':True}))['expressions'][0]['active'])
                            self.assertEqual(len((await req('HotkeysInCurrentModelRequest'))['availableHotkeys']),2)
                            if backend=='rust':
                                key=hashlib.sha256(b'halo').hexdigest()[:32]
                                other=hashlib.sha256(b'halo-alt').hexdigest()[:32]
                                asset=hashlib.sha256(b'Halo.exp3.json').hexdigest()[:32]
                                async def ui_wait(path,check):
                                    for _ in range(100):
                                        if path.exists() and check(path.read_text()): return path.read_text()
                                        await asyncio.sleep(.05)
                                    self.fail(f'UI response timeout: {path}')
                                # Plugin-defined inputs must be discoverable even before they have a mapping.
                                inputs_file=ipc/'input-ui.tsv'
                                def input_rows(t):
                                    return {r[1]:r for line in t.splitlines()[1:] if len(r:=line.split('\t'))==10}
                                await ui_wait(inputs_file,lambda t:'FacePositionX' in input_rows(t))
                                if iteration==0 and backend=='rust':
                                    await req('ParameterCreationRequest',dict(parameterName='BodyPositionX',min=-1,max=1,defaultValue=0))
                                    text=await ui_wait(inputs_file,lambda t:'BodyPositionX' in input_rows(t))
                                    self.assertEqual(input_rows(text)['BodyPositionX'][2:4],['plugin','VBridger'])
                                    self.assertEqual(input_rows(text)['BodyPositionX'][-2:],['0','0'])
                                    await req('InjectParameterDataRequest',dict(parameterValues=[dict(id='BodyPositionX',value=.75)]))
                                    text=await ui_wait(inputs_file,lambda t:input_rows(t).get('BodyPositionX',[])[-2:]==['1','0'])
                                    self.assertAlmostEqual(float(input_rows(text)['BodyPositionX'][7]),.75)
                                    with self.assertRaises(BlockingIOError):rx.recv(16384)
                                    # Applying a map is represented by a new renderer snapshot.
                                    original=snap.read_text();snap.write_text(original+'M\tBodyPositionX\tHaloToggle\t-1\t1\t0\t6\n')
                                    await ui_wait(inputs_file,lambda t:input_rows(t).get('BodyPositionX',[])[-1:]==['1'])
                                    await req('InjectParameterDataRequest',dict(parameterValues=[dict(id='BodyPositionX',value=.25)]))
                                    self.assertEqual(await packet(),'set plugin 1000 BodyPositionX 0.25')
                                    await ui_wait(inputs_file,lambda t:input_rows(t).get('BodyPositionX',[])[-2:]==['0','1'])
                                    snap.write_text(original)
                                    await ui_wait(inputs_file,lambda t:input_rows(t).get('BodyPositionX',[])[-1:]==['0'])
                                    await req('ParameterDeletionRequest',dict(parameterName='BodyPositionX'))
                                    await ui_wait(inputs_file,lambda t:'BodyPositionX' not in input_rows(t))
                                catalog=ipc/'expression-ui.tsv';result=ipc/'expression-result.txt'
                                await ui_wait(catalog,lambda t: f'E\t{asset}\t1\tHalo.exp3.json' in t and f'H\t{key}\t' in t)
                                async def ui_command(text,expected='OK'):
                                    result.unlink(missing_ok=True)
                                    tmp=ipc/'expression-request.tmp';tmp.write_text(text+'\n');tmp.rename(ipc/'expression-request.tsv')
                                    self.assertEqual((await ui_wait(result,lambda t: bool(t.strip()))).strip(),expected)
                                if iteration==0:
                                    self.assertIn('L\t1\n',catalog.read_text())
                                    await ui_command('import-vts\tmodel','Imported: 2 | Existing: 0 | Conflicts: 0 | Unsupported: 0 | No key: 0')
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t{0x10000+325}\t' in t and f'H\t{other}\t{0x10000+3*512+85}\t' in t)
                                    self.assertEqual(json.loads(state.with_suffix('.hotkeys.json').read_text())['global'],[])
                                    await ui_command('import-vts\tmodel','Imported: 0 | Existing: 2 | Conflicts: 0 | Unsupported: 0 | No key: 0')
                                    await ui_command(f'bind\t{key}\t{0x10000+53}')
                                    await ui_command('import-vts\tmodel','Imported: 0 | Existing: 2 | Conflicts: 0 | Unsupported: 0 | No key: 0')
                                    self.assertEqual(json.loads(state.with_suffix('.hotkeys.json').read_text())['bindings'][key],0x10000+53)
                                    self.assertEqual((src/'Test.vtube.json').read_bytes(),source_config)
                                    modelid=(await req('CurrentModelRequest'))['modelID']
                                    state.with_suffix('.hotkeys.json').write_text(json.dumps({'schema':'kawari-hotkeys-1','modelID':modelid,'bindings':{key:17}}))
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t{0x10000+2*512+294}\t' in t)
                                    await ui_command(f'bind\t{key}\t{0x10000+325}')
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t{0x10000+325}\t' in t)
                                    await ui_command(f'bind\t{other}\t{0x10000+325}','Shortcut already assigned')
                                    await ui_command(f'bind\t{other}\t{0x10000+53}')
                                    await ui_wait(catalog,lambda t: f'H\t{other}\t{0x10000+53}\t' in t)
                                    await ui_command(f'bind\t{key}\t4294967295','Invalid shortcut key')
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t{0x10000+325}\t' in t)
                                    await ui_command(f'bind\t{key}\t0')
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t0\t' in t)
                                    await ui_command(f'bind\t{key}\t{0x10000+3*512+85}')
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t{0x10000+3*512+85}\t' in t)
                                    await ui_command(f'global\t{key}\t1')
                                    await ui_wait(catalog,lambda t:f'S\t{key}\n' in t and 'Global shortcuts disabled for this run' in t)
                                    self.assertIn(key,json.loads(state.with_suffix('.hotkeys.json').read_text())['global'])
                                    await ui_command(f'bind\t{key}\t{0x10000+294}','Disable Global before changing the shortcut')
                                elif os.environ.get('STUDIO_API_TEST_FIRST',BACKEND)=='rust':
                                    await ui_wait(catalog,lambda t: f'H\t{key}\t{0x10000+3*512+85}\t' in t and f'S\t{key}\n' in t)
                                    await ui_command(f'global\t{key}\t0')
                                    await ui_wait(catalog,lambda t:f'S\t{key}\n' not in t)
                                    self.assertNotIn(key,json.loads(state.with_suffix('.hotkeys.json').read_text())['global'])
                                await ui_command(f'toggle\t{asset}')
                                self.assertTrue((await packet()).startswith('expr off '))
                                self.assertFalse((await req('ExpressionStateRequest'))['expressions'][0]['active'])
                                await ui_command(f'trigger\t{key}')
                                self.assertTrue((await packet()).startswith('expr on '))
                                self.assertTrue((await req('ExpressionStateRequest'))['expressions'][0]['active'])
                            if iteration==1:
                                await req('ExpressionActivationRequest',{'expressionFile':'Halo.exp3.json','active':False})
                                self.assertTrue((await packet()).startswith('expr off '))
                                self.assertEqual(json.loads(state.read_text())['activeExpressions'],[])
                    finally:
                        child.terminate()
                        try: child.wait(timeout=3)
                        except subprocess.TimeoutExpired: child.kill();child.wait()
            finally: stop.set();thread.join();rx.close()

if __name__=='__main__': unittest.main()
