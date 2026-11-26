"""Supervisor hot switch contract: same renderer and WebSocket session, fresh IPC and model context.
Uses a mock viewer, so actual Vulkan/OBS behavior still requires hardware testing.
"""
import json, os, signal, socket, subprocess, sys, tempfile, time, unittest
from pathlib import Path
from websockets.sync.client import connect
ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'api-rust/target/release/studio'
API=BIN.with_name('studio-vts-api')

@unittest.skipUnless(BIN.is_file() and API.is_file(), 'Run scripts/build-core.sh first')
class HotSwitch(unittest.TestCase):
    def setUp(self):
        help_text=subprocess.check_output([str(BIN),"--help"],text=True,timeout=5)
        self.assertIn("persistent API protocol 1",help_text,
                      "Stale Application Core: run ./scripts/build-core.sh after extracting P0.5d1")

        self.assertIn("persistent API protocol 1",subprocess.check_output([str(API),"--help"],text=True,timeout=5))

    def test_commit_reject_and_resume(self):
        with tempfile.TemporaryDirectory(prefix='st-hot-') as td:
            root=Path(td); models=root/'Live2DModels';models.mkdir()
            def create(name):
                d=models/name;d.mkdir();(d/'m.moc3').touch();(d/'t.png').touch()
                expression=name+'.exp3.json'
                (d/expression).write_text(json.dumps({'Type':'Live2D Expression','Parameters':[{'Id':'Param'+name,'Value':1,'Blend':'Overwrite'}]}))
                p=d/f'{name}.model3.json';p.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'m.moc3','Textures':['t.png'],'Expressions':[{'Name':name,'File':expression}]}}));return p.resolve()
            first,second,reject=map(create,['First','Second','Reject'])
            fake=root/'viewer';fake.write_text(f'#!{sys.executable}\n'+'''import json,os,socket,tempfile,time
from pathlib import Path
mail=Path(os.environ['STUDIO_LIBRARY_DIR']);root=Path(os.environ['FAKE_EVENTS']).parent
assert mail.stat().st_mode & 0o777 == 0o700, f'Non-private control mailbox: {oct(mail.stat().st_mode & 0o777)}'
assert mail.stat().st_uid == os.getuid()
assert os.environ['STUDIO_HOT_SWITCH']=='1'
assert os.environ['STUDIO_HOT_SWITCH_PROTOCOL']=='1'
current=os.environ['STUDIO_ORIGINAL_MODEL']; ipc=None;s=None
while True:
    if ipc is None:
        ipc=tempfile.TemporaryDirectory(prefix='ipc-',dir=root);p=Path(ipc.name)
        s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.bind(str(p/'input.sock'));s.setblocking(False)
        with open(os.environ['FAKE_EVENTS'],'a') as f:f.write(json.dumps({'mail':str(mail),'model':current,'pid':os.getpid(),'socket':str(p/'input.sock')})+'\\n')
        print('[parameters] socket: '+str(p/'input.sock'),flush=True)
    (p/'parameters.tmp').write_text('P\\tParam'+Path(current).name.split('.')[0]+'\\t-30\\t30\\t0\\t0\\nM\\tFaceAngleX\\tParam'+Path(current).name.split('.')[0]+'\\t-40\\t40\\t-30\\t30\\n')
    (p/'parameters.tmp').replace(p/'parameters.tsv')
    try:
        packet=s.recv(16384).decode()
        with open(root/'packets','a') as f:f.write(json.dumps({'model':current,'packet':packet})+'\\n')
    except BlockingIOError:pass
    load=mail/'load.txt'
    if load.exists():
        original,runtime,mapping,editable=load.read_text().splitlines();load.unlink()
        assert Path(runtime).is_file() and Path(mapping).is_file() and editable=='1'
        if 'Second.model3.json' in original:
            deadline=time.monotonic()+5
            while not (mail/'test-release').exists() and time.monotonic()<deadline:time.sleep(.02)
            assert (mail/'test-release').exists(), 'test did not release switch'
            (mail/'test-release').unlink()
        if 'Reject.model3.json' in original:
            print('[model-switch] rejected: simulated decode failure',flush=True)
        else:
            s.close();ipc.cleanup();ipc=None;current=original
    time.sleep(.025)
''');fake.chmod(0o700)
            events=root/'events';log=root/'log';data=root/'data';catalog=data/'kawari/library.json'
            env=dict(os.environ,XDG_DATA_HOME=str(data),STUDIO_VIEWER_BIN=str(fake),FAKE_EVENTS=str(events),STUDIO_HOT_SWITCH='1');env.pop('STUDIO_HOT_SWITCH_PROTOCOL',None)
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            base=[str(BIN),'--allow-plugin','VBridger','--port',str(port),'--startup-timeout','8','--switch-timeout','8']
            def rows():return [json.loads(x) for x in events.read_text().splitlines() if x] if events.exists() else []
            def wait(child,predicate,phase,renderer_pid=None):
                deadline=time.monotonic()+22
                try:
                    while time.monotonic()<deadline:
                        if child.poll() is not None:self.fail(f'{phase}: supervisor exited ({child.returncode})\n'+log.read_text())
                        try:
                            if renderer_pid is not None and any(r['pid']!=renderer_pid for r in rows()):
                                self.fail(f'{phase}: renderer restarted instead of hot switching\n'+log.read_text())
                            if predicate():return
                        except (FileNotFoundError,json.JSONDecodeError):pass
                        time.sleep(.05)
                except KeyboardInterrupt:
                    print(f'\nInterrupted during {phase}:\n'+log.read_text(),file=sys.stderr,flush=True)
                    raise
                self.fail(f'Timed out during {phase}:\n'+log.read_text())
            def select(mail,path):
                for name in ('loaded.txt','error.txt'):(mail/name).unlink(missing_ok=True)
                tmp=mail/'select.tmp';tmp.write_text(str(path));tmp.replace(mail/'select.txt')
            identity={'pluginName':'VBridger','pluginDeveloper':'Test Dev'}
            def req(ws,kind,data=None):
                ws.send(json.dumps(dict(apiName='VTubeStudioPublicAPI',apiVersion='1.0',messageType=kind,requestID='t',data=data or {})))
                r=json.loads(ws.recv(timeout=3));self.assertEqual(r['messageType'],kind.removesuffix('Request')+'Response',r);return r['data']
            def check_api(ws,expected,token):
                self.assertTrue(req(ws,'APIStateRequest')['currentSessionAuthenticated'])
                self.assertEqual(req(ws,'CurrentModelRequest')['live2DModelName'],expected.name)
                self.assertEqual(req(ws,'Live2DParameterListRequest')['parameters'][0]['name'],'Param'+expected.name.split('.')[0])
                self.assertTrue(any(p['name']=='MouthCustom' for p in req(ws,'InputParameterListRequest')['customParameters']))
                self.assertEqual(req(ws,'AuthenticationTokenRequest',identity)['authenticationToken'],token)
            def input_catalog_row(name):
                p=Path(rows()[-1]['socket']).parent/'input-ui.tsv'
                if not p.exists():return []
                return next((r for line in p.read_text().splitlines()[1:] if len(r:=line.split('\t'))==10 and r[1]==name),[])
            def packets():return [json.loads(x) for x in (root/'packets').read_text().splitlines()] if (root/'packets').exists() else []
            def inject(ws,value):req(ws,'InjectParameterDataRequest',{'faceFound':True,'parameterValues':[{'id':'FaceAngleX','value':value},{'id':'MouthCustom','value':.5}]})
            def check_packet(child,model,value):wait(child,lambda:any(p['model']==str(model) and p['packet']==f'set plugin 1000 FaceAngleX {value}' for p in packets()),'tracking routed to new IPC')
            with log.open('w') as out:
                # Reproduce standard Linux permissions regardless of the test runner umask.
                child=subprocess.Popen(base+[str(first)],env=env,stdout=out,stderr=out,umask=0o022)
                try:
                    wait(child,lambda:'[studio] Ready: renderer + VTS API' in log.read_text(),'initial startup');mail=Path(rows()[0]['mail']);pid=rows()[0]['pid']
                    with connect(f'ws://127.0.0.1:{port}',open_timeout=3,close_timeout=1) as ws:
                        token=req(ws,'AuthenticationTokenRequest',identity)['authenticationToken']
                        self.assertTrue(req(ws,'AuthenticationRequest',dict(identity,authenticationToken=token))['authenticated'])
                        req(ws,'ParameterCreationRequest',dict(parameterName='MouthCustom',min=-1,max=1,defaultValue=0))
                        check_api(ws,first,token);inject(ws,10);check_packet(child,first,10)
                        wait(child,lambda:input_catalog_row("MouthCustom")[-2:]==["1","0"],"unmapped plugin catalog",pid)
                        req(ws,'ExpressionActivationRequest',{'expressionFile':'First.exp3.json','active':True})
                        select(mail,reject);wait(child,lambda:(mail/'error.txt').is_file(),'rejected candidate',pid);self.assertEqual(len(rows()),1)
                        self.assertIn('simulated decode failure',(mail/'error.txt').read_text());check_api(ws,first,token)
                        self.assertEqual(json.loads(catalog.read_text())['lastModel'],str(first))
                        self.assertTrue(req(ws,'ExpressionStateRequest')['expressions'][0]['active'])
                        select(mail,second);wait(child,lambda:log.read_text().count('[api-control] ok pause')>=2,'pause before switch',pid)
                        self.assertTrue(req(ws,'APIStateRequest')['currentSessionAuthenticated']);self.assertFalse(req(ws,'CurrentModelRequest')['modelLoaded']);inject(ws,99);(mail/'test-release').touch()
                        wait(child,lambda:(mail/'loaded.txt').is_file(),'switch to second',pid);self.assertEqual(len(rows()),2)
                        self.assertEqual(rows()[0]['pid'],rows()[1]['pid']);self.assertNotEqual(rows()[0]['socket'],rows()[1]['socket'])
                        self.assertFalse(Path(rows()[0]['socket']).exists());check_api(ws,second,token)
                        wait(child,lambda:input_catalog_row("MouthCustom")[-2:]==["0","0"],"new model catalog without old samples",pid)
                        inject(ws,20);check_packet(child,second,20)
                        self.assertFalse(any('FaceAngleX 99' in p['packet'] for p in packets()))
                        self.assertEqual(json.loads(catalog.read_text())['lastModel'],str(second))
                        second_expr=req(ws,'ExpressionStateRequest')['expressions'];self.assertEqual(second_expr[0]['file'],'Second.exp3.json');self.assertFalse(second_expr[0]['active'])
                        req(ws,'ExpressionActivationRequest',{'expressionFile':'Second.exp3.json','active':True})
                        select(mail,first);wait(child,lambda:(mail/'loaded.txt').is_file(),'switch back to first',pid);self.assertEqual(len(rows()),3);check_api(ws,first,token);inject(ws,30);check_packet(child,first,30)
                        self.assertEqual({r['pid'] for r in rows()},{rows()[0]['pid']});self.assertEqual(len({r['socket'] for r in rows()}),3)
                        self.assertEqual(log.read_text().count('[vts-api] Listening on '),1)
                        first_expr=req(ws,'ExpressionStateRequest')['expressions'];self.assertEqual(first_expr[0]['file'],'First.exp3.json');self.assertTrue(first_expr[0]['active'])
                        token_files=list((data/'kawari/profiles').glob('*/api-tokens.json'));self.assertEqual(len(token_files),2)
                        for f in token_files:
                            self.assertEqual(f.stat().st_mode & 0o777,0o600);self.assertEqual(json.loads(f.read_text())['tokens'][0]['token'],token)
                    with connect(f'ws://127.0.0.1:{port}',open_timeout=3,close_timeout=1) as ws:
                        self.assertTrue(req(ws,'AuthenticationRequest',dict(identity,authenticationToken=token))['authenticated']);check_api(ws,first,token)
                finally:
                    if child.poll() is None:child.send_signal(signal.SIGTERM)
                    try:child.wait(timeout=10)
                    except subprocess.TimeoutExpired:child.kill();child.wait();self.fail('Studio did not terminate')

if __name__=='__main__':unittest.main()
