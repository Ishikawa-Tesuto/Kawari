"""Native helper tests with a real local WebSocket; no Twitch credentials needed."""
import json, os, stat, subprocess, tempfile, time, threading, unittest, shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class NativeTwitch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import websockets.sync.server
        except ImportError:
            raise unittest.SkipTest('python-websockets is needed for tests only')
        if not shutil.which('pkg-config') or subprocess.run(['pkg-config','--exists','libcurl >= 8.14','json-c']).returncode:
            raise unittest.SkipTest('native helper build dependencies missing')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary=Path(cls.tmp.name)/'native-test'
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','libcurl','json-c'],text=True).split()
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(ROOT/'tests/test_native_twitch.cpp'),*flags,'-o',str(cls.binary)],check=True)
        cls.helper=ROOT/'build/kawari-twitch'
        if not cls.helper.is_file():
            subprocess.run(['bash',str(ROOT/'scripts/build-twitch.sh')],check=True)
    def test_real_local_ws_and_migration(self):
        from websockets.sync.server import serve
        requests=[];calls=[]
        from websockets.exceptions import ConnectionClosed
        def server(ws):
            try:
                if ws.request.path in ('/event-initial','/event-replacement'):
                    first=ws.request.path=='/event-initial'
                    ws.send(json.dumps({'metadata':{'message_type':'session_welcome'},'payload':{'session':{'id':'initial' if first else 'replacement','keepalive_timeout_seconds':10}}}))
                    if first:
                        ws.send(json.dumps({'metadata':{'message_type':'session_reconnect'},'payload':{'session':{'reconnect_url':'wss://eventsub.wss.twitch.tv/ws?reconnect=mock'}}}))
                    for _ in ws:
                        pass
                    return
                serve_messages(ws)
            except ConnectionClosed:
                pass
        def serve_messages(ws):
            for text in ws:
                r=json.loads(text);kind=r['messageType'];data=r['data'];requests.append(kind)
                if kind=='AuthenticationRequest':
                    assert data['authenticationToken']=='test-credential';response={'authenticated':True}
                elif kind=='AuthenticationTokenRequest':
                    time.sleep(.15);response={'authenticationToken':'test-credential'}
                elif kind=='CurrentModelRequest':response={'modelLoaded':True,'modelID':'A','modelName':'TesutoXXI'}
                elif kind=='ExpressionStateRequest':response={'modelID':'A','expressions':[{'file':'SharkTail - Toggle.exp3.json','name':'Tail'},{'file':'Halo.exp3.json','name':'Halo'}]}
                elif kind=='KawariRewardExpressionRequest':calls.append(data);response={}
                else:raise AssertionError(kind)
                out=json.dumps({'requestID':r['requestID'],'messageType':kind.removesuffix('Request')+'Response','data':response})
                # Fragmented replies exercise native frame assembly.
                ws.send([out[:len(out)//2],out[len(out)//2:]])
        with tempfile.TemporaryDirectory() as d,serve(server,'127.0.0.1',0) as s:
            t=threading.Thread(target=s.serve_forever,daemon=True);t.start()
            p=subprocess.run([str(self.binary),d,str(s.socket.getsockname()[1])],text=True,capture_output=True,timeout=15)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertEqual(calls,[{'expressionFile':'SharkTail - Toggle.exp3.json','mode':'toggle','duration':0,'modelID':'A'},{'expressionFile':'Halo.exp3.json','mode':'timed','duration':60,'modelID':'A'},{'expressionFile':'Halo.exp3.json','mode':'timed','duration':12,'modelID':'A'},{'expressionFile':'Halo.exp3.json','mode':'timed','duration':60,'modelID':'A'}])
            self.assertEqual(requests.count('AuthenticationRequest'),3)
            self.assertEqual(requests.count('AuthenticationTokenRequest'),1)
            self.assertEqual((Path(d)/'bindings.json').stat().st_mode&0o777,0o600)
    def test_optional_helper_and_lock(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);mail=root/'mail';mail.mkdir(mode=0o700);data=root/'data';twitch=data/'twitch';twitch.mkdir(mode=0o700,parents=True)
            oauth=twitch/'oauth.json';oauth.write_text(json.dumps({'client_id':'client123456789','access_token':'PRIVATE','refresh_token':'REFRESH'}));oauth.chmod(0o600)
            def wait(pred):
                end=time.monotonic()+4
                while time.monotonic()<end:
                    if pred():return
                    time.sleep(.05)
                self.fail('Timed out waiting for helper')
            with subprocess.Popen([str(self.helper),str(mail),str(data),'8001'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL) as p:
                try:
                    snapshot=mail/'twitch-ui.tsv';wait(snapshot.exists);self.assertIn('Disconnected',snapshot.read_text());self.assertNotIn('PRIVATE',snapshot.read_text());self.assertNotIn('REFRESH',snapshot.read_text())
                    second=subprocess.run([str(self.helper),str(mail),str(data),'8001'],capture_output=True,text=True,timeout=4)
                    self.assertNotEqual(second.returncode,0);self.assertIn('Another Twitch bridge',second.stderr)
                    def command(s):
                        f=mail/'request.tmp';f.write_text(s+'\n');f.chmod(0o600);f.rename(mail/'twitch-request.tsv')
                    command('connect\tbad');wait(lambda:'Invalid Client ID' in snapshot.read_text())
                    command('logout');wait(lambda:not oauth.exists());wait(lambda:'Local OAuth removed' in snapshot.read_text())
                    self.assertEqual(snapshot.stat().st_mode&0o777,0o600)
                finally:p.terminate();p.wait(timeout=4)
    def test_gui_and_supervisor_contract(self):
        ui=(ROOT/'native/studio_ui.cpp').read_text();core=(ROOT/'api-rust/src/bin/studio.rs').read_text()
        self.assertIn('twitch_panel();',ui);self.assertIn('twitch-request',ui);self.assertIn('const std::string key=t.model',ui)
        self.assertIn('build/kawari-twitch',core);self.assertIn('KAWARI_TWITCH_NATIVE',core)
        self.assertIn('Native integration stopped',core)
if __name__=='__main__':unittest.main()
