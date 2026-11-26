"""End-to-end library switching via the same private mailbox as the GUI."""
import json, os, signal, socket, subprocess, sys, tempfile, time, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'api-rust/target/release/studio'

@unittest.skipUnless(BIN.is_file(), 'Run scripts/build-core.sh first')
class ModelLibrary(unittest.TestCase):
    def test_switch_and_resume(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td); models=root/'Live2DModels'; models.mkdir()
            def create(name):
                d=models/name; d.mkdir(); (d/'m.moc3').touch(); (d/'t.png').touch()
                p=d/f'{name}.model3.json'; p.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'m.moc3','Textures':['t.png']}}));return p.resolve()
            first=create('First Model');second=create('Second Model');bad=models/'Bad.model3.json';bad.write_text('{}')
            fake=root/'fake-viewer';fake.write_text(f'#!{sys.executable}\n'+'''import os,socket,tempfile,time,json
from pathlib import Path
mail=Path(os.environ['STUDIO_LIBRARY_DIR'])
with open(os.environ['FAKE_EVENTS'],'a') as f:
    f.write(json.dumps({'mail':str(mail),'model':os.environ['STUDIO_ORIGINAL_MODEL'],'pid':os.getpid()})+'\\n')
with tempfile.TemporaryDirectory(prefix='ipc-', dir=os.path.dirname(os.environ['FAKE_EVENTS'])) as d:
    p=Path(d);s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.bind(str(p/'input.sock'))
    print('[parameters] socket: '+str(p/'input.sock'),flush=True)
    while True:
        (p/'parameters.tsv').write_text('P\\tParamAngleX\\t-30\\t30\\t0\\t0\\n');time.sleep(.05)
''');fake.chmod(0o700)
            events=root/'events';log=root/'log';data=root/'data';catalog=data/'kawari/library.json'
            env=dict(os.environ,XDG_DATA_HOME=str(data),STUDIO_VIEWER_BIN=str(fake),FAKE_EVENTS=str(events),STUDIO_HOT_SWITCH="0")
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            base=[str(BIN),'--allow-plugin','VBridger','--port',str(port),'--startup-timeout','8']
            def rows():return [json.loads(x) for x in events.read_text().splitlines() if x] if events.exists() else []
            def wait(child, predicate):
                for _ in range(240):
                    if child.poll() is not None:self.fail(log.read_text())
                    try:
                        if predicate():return
                    except (FileNotFoundError,json.JSONDecodeError):pass
                    time.sleep(.05)
                self.fail('Timed out: '+log.read_text())
            def ready_count():return log.read_text().count('[studio] Ready: renderer + VTS API')
            def select(mail,path):
                tmp=mail/'select.tmp';tmp.write_text(str(path));tmp.replace(mail/'select.txt')
            def stop(child):
                if child.poll() is None:
                    child.send_signal(signal.SIGTERM)
                    try:child.wait(timeout=10)
                    except subprocess.TimeoutExpired:child.kill();child.wait();self.fail('Studio did not terminate')
            with log.open('w') as out:
                child=subprocess.Popen(base+[str(first)],env=env,stdout=out,stderr=out)
                try:
                    wait(child,lambda:ready_count()==1);mail=Path(rows()[0]['mail'])
                    listed=(mail/'models.txt').read_text().splitlines();self.assertIn(str(first),listed);self.assertIn(str(second),listed);self.assertNotIn(str(bad),listed)
                    select(mail,bad);wait(child,lambda:(mail/'error.txt').is_file());self.assertEqual(len(rows()),1)
                    select(mail,second);wait(child,lambda:ready_count()==2)
                    self.assertEqual(rows()[1]['model'],str(second));self.assertEqual(json.loads(catalog.read_text())['lastModel'],str(second))
                    with self.assertRaises(ProcessLookupError):os.kill(rows()[0]['pid'],0)
                finally:stop(child)
                child=subprocess.Popen(base,env=env,stdout=out,stderr=out)
                try:
                    wait(child,lambda:ready_count()==3);self.assertEqual(rows()[2]['model'],str(second))
                finally:stop(child)
            self.assertEqual(len(list((data/'kawari/profiles').iterdir())),2)

if __name__=='__main__':unittest.main()
