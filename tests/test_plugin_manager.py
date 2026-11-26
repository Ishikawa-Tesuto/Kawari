"""Native API authorization lifecycle with fake renderer socket."""
import json,os,socket,subprocess,tempfile,time,unittest
from pathlib import Path
from websockets.sync.client import connect
ROOT=Path(__file__).resolve().parents[1];BIN=ROOT/'api-rust/target/release/studio-vts-api'
@unittest.skipUnless(BIN.is_file(),'Build Rust API first')
class Manager(unittest.TestCase):
    def test_grant_revoke_restart(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);ipc=root/'ipc';ipc.mkdir(mode=0o700);s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.bind(str(ipc/'input.sock'));self.addCleanup(s.close)
            with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
            store=root/'plugins.json';ident={'pluginName':'Test Plugin','pluginDeveloper':'Test Developer'}
            def start():
                c=subprocess.Popen([str(BIN),'--socket',str(ipc/'input.sock'),'--plugin-store',str(store),'--port',str(port)],stdout=subprocess.DEVNULL);self.addCleanup(lambda: c.poll() is None and c.kill());return c
            def wait(fn):
                end=time.monotonic()+5
                while time.monotonic()<end:
                    try:
                        v=fn()
                        if v:return v
                    except (FileNotFoundError,OSError):pass
                    time.sleep(.05)
                self.fail('Timed out')
            def send(ws,k,data):ws.send(json.dumps({'apiName':'VTubeStudioPublicAPI','apiVersion':'1.0','requestID':'a','messageType':k,'data':data}))
            def recv(ws):return json.loads(ws.recv(timeout=5))
            def ask(ws,k,data):send(ws,k,data);return recv(ws)
            def row(state):return next((x.split('\t') for x in (ipc/'plugins-ui.tsv').read_text().splitlines()[1:] if x.split('\t')[1]==state),None)
            def action(key,mode):
                p=ipc/'plugins-request.tmp';p.write_text(mode+'\t'+key+'\n');p.replace(ipc/'plugins-request.tsv')
            c=start()
            try:
                with wait(lambda:connect(f'ws://127.0.0.1:{port}',open_timeout=.3,close_timeout=.2)) as ws:
                    send(ws,'AuthenticationTokenRequest',ident);r=wait(lambda:row('Pending'));action(r[0],'allow');token=recv(ws)['data']['authenticationToken'];self.assertTrue(ask(ws,'AuthenticationRequest',dict(ident,authenticationToken=token))['data']['authenticated']);wait(lambda:row('Allowed') and row('Allowed')[2]=='1');self.assertEqual(store.stat().st_mode&0o777,0o600)
                    action(r[0],'block');wait(lambda:row('Blocked'));self.assertFalse(ask(ws,'APIStateRequest',{})['data']['currentSessionAuthenticated']);self.assertEqual(ask(ws,'CurrentModelRequest',{})['messageType'],'APIError');self.assertEqual(ask(ws,'AuthenticationTokenRequest',ident)['messageType'],'APIError')
                    action(r[0],'allow');wait(lambda:row('Allowed'));self.assertFalse(ask(ws,'AuthenticationRequest',dict(ident,authenticationToken=token))['data']['authenticated']);new=ask(ws,'AuthenticationTokenRequest',ident)['data']['authenticationToken'];self.assertNotEqual(new,token)
            finally:c.terminate();c.wait(timeout=5)
            c=start()
            try:
                with wait(lambda:connect(f'ws://127.0.0.1:{port}',open_timeout=.3,close_timeout=.2)) as ws:self.assertTrue(ask(ws,'AuthenticationRequest',dict(ident,authenticationToken=new))['data']['authenticated'])
            finally:c.terminate();c.wait(timeout=5)
if __name__=='__main__':unittest.main()
