"""Settings wire-format and real supervisor persistence integration."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'api-rust/target/release/studio'
API=BIN.with_name('studio-vts-api')
DEFAULT='kawari-broadcast-1\n1920 1080 60 60 1024 1 0 0\n'
SAVED='kawari-broadcast-2\n1280 720 120 30 512 2 1 1 1\n'

class WireFormat(unittest.TestCase):
    def test_cpp_wire_format(self):
        src=r'''
#include "studio_ui_layout.hpp"
#include <cassert>
using namespace studio;
int main(){
    UiBroadcast d,s;
    assert(ui_write_broadcast(d)=="kawari-broadcast-1\n1920 1080 60 60 1024 1 0 0\n");
    std::istringstream in("kawari-broadcast-1\n1280 720 120 30 512 2 1 1\n");
    assert(ui_read_broadcast(in,s)&&s.valid()&&s.width==1280&&s.height==720&&s.fps==120&&s.bfps==30&&s.mask==512&&s.ssaa==2&&s.no_preview&&s.stats);
    assert(s.restart_diff(d));
    auto t=d;t.fps=120;t.stats=true;assert(!t.restart_diff(d)&&!(t==d));
    t.pause_unfocused_preview=true;
    const auto changed=ui_write_broadcast(t);
    assert(changed=="kawari-broadcast-2\n1920 1080 120 60 1024 1 0 1 1\n");
    std::istringstream updated(changed);
    assert(ui_read_broadcast(updated,s)&&s==t&&!s.restart_diff(d));
    for(const char* text:{"kawari-broadcast-1 319 720 60 60 1024 1 0 0","kawari-broadcast-1 7680 4320 60 60 1024 2 0 0","kawari-broadcast-1 1280 720 61 60 1024 1 0 0","kawari-broadcast-1 1280 720 60 60 1024 1 2 0","kawari-broadcast-1 1280 720 60 60 1024 1 0 0 extra","kawari-broadcast-2 1280 720 60 60 1024 1 0 0","kawari-broadcast-2 1280 720 60 60 1024 1 0 0 2"}){
        auto old=s;std::istringstream bad(text);assert(!ui_read_broadcast(bad,s)&&s==old);
    }
}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'test.cpp').write_text(src)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

@unittest.skipUnless(BIN.is_file() and API.is_file(),'Build Rust Core first')
class Persistence(unittest.TestCase):
    def test_save_and_restart(self):
        with tempfile.TemporaryDirectory(prefix='kawari-broadcast-') as td:
            base=Path(td);original=base/'original';original.mkdir();model=original/'Test.model3.json';(original/'body.moc3').touch();(original/'tex.png').touch()
            model.write_text(json.dumps({'Version':3,'FileReferences':{'Moc':'body.moc3','Textures':['tex.png']}}))
            fake=base/'viewer';info=base/'viewer.json';log=base/'log'
            fake.write_text(f'#!{sys.executable}\n'+'''import json, os, socket, tempfile, time
from pathlib import Path
p=Path(tempfile.mkdtemp(prefix='ipc-',dir=os.environ['TMPDIR']))
s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.bind(str(p/'input.sock'))
(p/'parameters.tsv').write_text('P\\tParamX\\t-1\\t1\\t0\\t0\\n')
Path(os.environ['TEST_INFO']).write_text(json.dumps(dict(pid=os.getpid(),mail=os.environ['STUDIO_LIBRARY_DIR'],settings=[os.environ[x] for x in ('STUDIO_BROADCAST_WIDTH','STUDIO_BROADCAST_HEIGHT','STUDIO_FPS','STUDIO_BROADCAST_FPS','STUDIO_MASK_SIZE','STUDIO_SSAA','STUDIO_NO_PREVIEW','STUDIO_STATS','STUDIO_PAUSE_UNFOCUSED_PREVIEW')])))
print('[parameters] socket: '+str(p/'input.sock'),flush=True)
while True:
    (p/'parameters.tsv').touch()
    time.sleep(.05)
''');fake.chmod(0o700)
            env=dict(os.environ,XDG_DATA_HOME=str(base/'data'),TMPDIR=td,STUDIO_VIEWER_BIN=str(fake),TEST_INFO=str(info),STUDIO_HOT_SWITCH='0')
            for name in ('STUDIO_MASK_SIZE','STUDIO_SSAA'):env.pop(name,None)
            config=base/'data/kawari/broadcast.conf'
            def wait(child,pred):
                end=time.monotonic()+12
                while time.monotonic()<end:
                    if child.poll() is not None:self.fail(log.read_text())
                    if pred():return
                    time.sleep(.05)
                self.fail('Timeout: '+log.read_text())
            def launch(extra,action):
                info.unlink(missing_ok=True)
                with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
                with log.open('w') as out:
                    child=subprocess.Popen([str(BIN),'--allow-plugin','Test','--port',str(port),*extra,str(model)],env=env,stdout=out,stderr=out)
                    try:
                        wait(child,lambda:'[studio] Ready: renderer + VTS API' in log.read_text())
                        data=json.loads(info.read_text());mail=Path(data['mail']);action(child,data,mail)
                    finally:
                        child.terminate();child.wait(timeout=10)
            def first(child,data,mail):
                self.assertEqual((mail/'broadcast-current.tsv').read_text(),DEFAULT)
                def request(text):
                    result=mail/'broadcast-result.txt';result.unlink(missing_ok=True)
                    tmp=mail/'broadcast-save.tmp';tmp.write_text(text);tmp.rename(mail/'broadcast-save.tsv')
                    wait(child,result.exists);return result.read_text()
                self.assertEqual(request(SAVED),'ok');self.assertEqual(config.read_text(),SAVED)
                self.assertEqual((mail/'broadcast-current.tsv').read_text(),'kawari-broadcast-2\n1920 1080 120 60 1024 1 0 1 1\n')
                self.assertEqual(config.stat().st_mode&0o777,0o600)
                self.assertTrue(request('invalid').startswith('error:'));self.assertEqual(config.read_text(),SAVED)
                self.assertEqual(data['pid'],json.loads(info.read_text())['pid']);os.kill(data['pid'],0)
            launch([],first)
            launch([],lambda child,data,mail:self.assertEqual(data['settings'],['1280','720','120','30','512','2','1','1','1']))
            launch(['--preview','--keep-unfocused-preview','--no-stats','--broadcast-size','1920x1080','--fps','60','--mask-size','1024'],lambda child,data,mail:self.assertEqual(data['settings'],['1920','1080','60','30','1024','2','0','0','0']))
            self.assertEqual(config.read_text(),SAVED)

if __name__=='__main__':unittest.main()
