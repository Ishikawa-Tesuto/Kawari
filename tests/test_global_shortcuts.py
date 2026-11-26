"""Native X11 backend integration; Wayland requires a real portal/compositor."""
import ctypes as C
import os, pathlib, select, shutil, subprocess, tempfile, time, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]

def chord(key, mods=0): return str(0x10000 | (mods<<9) | key)

class GlobalShortcuts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('c++') or not shutil.which('pkg-config') or subprocess.run(['pkg-config','--exists','x11']).returncode:
            raise unittest.SkipTest('C++ and X11 development files required')
        cls.tmp=tempfile.TemporaryDirectory();cls.bin=pathlib.Path(cls.tmp.name)/'helper'
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(ROOT/'native/global_shortcuts.cpp'),'-lX11','-o',str(cls.bin)],check=True)
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def test_trigger_mapping(self):
        for key,mods,expected in [(85,3,'CTRL+SHIFT+u'),(325,0,'KP_5'),(53,0,'5'),(334,4,'ALT+KP_Add'),(294,8,'LOGO+F5'),(331,0,'KP_Divide')]:
            self.assertEqual(subprocess.check_output([self.bin,'--trigger-format',chord(key,mods)],text=True).strip(),expected)
    def test_x11_background_activation_and_release(self):
        if not shutil.which('Xvfb'): self.skipTest('Xvfb not installed')
        try: x=C.CDLL('libX11.so.6');xt=C.CDLL('libXtst.so.6')
        except OSError: self.skipTest('libXtst unavailable')
        r,w=os.pipe();server=subprocess.Popen(['Xvfb','-displayfd',str(w),'-screen','0','640x480x24','-nolisten','tcp'],pass_fds=(w,),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);os.close(w)
        proc=None;d=None
        try:
            self.assertTrue(select.select([r],[],[],5)[0],'Xvfb startup timeout');display=':'+os.read(r,128).decode().strip();env=dict(os.environ,DISPLAY=display,XDG_SESSION_TYPE='x11')
            x.XOpenDisplay.argtypes=[C.c_char_p];x.XOpenDisplay.restype=C.c_void_p;d=x.XOpenDisplay(display.encode());self.assertTrue(d)
            x.XStringToKeysym.argtypes=[C.c_char_p];x.XStringToKeysym.restype=C.c_ulong;x.XKeysymToKeycode.argtypes=[C.c_void_p,C.c_ulong];x.XKeysymToKeycode.restype=C.c_ubyte
            x.XSync.argtypes=[C.c_void_p,C.c_int];x.XCloseDisplay.argtypes=[C.c_void_p]
            xt.XTestFakeKeyEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong];xt.XTestFakeKeyEvent.restype=C.c_int
            def press(name,down):
                code=x.XKeysymToKeycode(d,x.XStringToKeysym(name.encode()));self.assertTrue(code);self.assertTrue(xt.XTestFakeKeyEvent(d,code,down,0));x.XSync(d,0)
            # Separate process registers only selected chords; no Kawari window exists.
            proc=subprocess.Popen([self.bin,'combo',chord(85,3),'Combo','pad',chord(325),'Pad','row',chord(53),'Row'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
            pending=bytearray()
            def read_line(timeout=3):
                end=time.monotonic()+timeout
                while b'\n' not in pending:
                    remain=end-time.monotonic()
                    if remain<=0 or not select.select([proc.stdout],[],[],remain)[0]:return None
                    block=os.read(proc.stdout.fileno(),4096)
                    if not block:return None
                    pending.extend(block)
                line,_,rest=pending.partition(b'\n');pending[:]=rest;return line.decode()
            self.assertEqual(read_line(),'STATUS X11: 3/3 registered')
            press('Control_L',1);press('u',1);press('u',0);press('Control_L',0)
            self.assertIsNone(read_line(.2),'Ctrl+U must not match Ctrl+Shift+U')
            press('Control_L',1);press('Shift_L',1);press('u',1);self.assertEqual(read_line(),'EVENT combo');time.sleep(.8);self.assertIsNone(read_line(.1),'holding key must not repeat');press('u',0);press('Shift_L',0);press('Control_L',0)
            press('KP_5',1);press('KP_5',0);self.assertEqual(read_line(),'EVENT pad');press('5',1);press('5',0);self.assertEqual(read_line(),'EVENT row')
            # A second client cannot grab the same key while the first helper owns it.
            conflict=subprocess.run([self.bin,'other',chord(53),'Other'],env=env,capture_output=True,text=True,timeout=4)
            self.assertEqual(conflict.returncode,1);self.assertIn('0/1 registered',conflict.stdout)
            proc.terminate();proc.wait(timeout=3);proc.stdout.close();proc.stderr.close();proc=None
            # The OS releases all grabs when helper exits.
            proc=subprocess.Popen([self.bin,'again',chord(53),'Again'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0);pending.clear();self.assertEqual(read_line(),'STATUS X11: 1/1 registered')
            press('5',1);press('5',0);self.assertEqual(read_line(),'EVENT again')
        finally:
            if proc:proc.terminate();proc.wait(timeout=3);proc.stdout.close();proc.stderr.close()
            if d:x.XCloseDisplay(d)
            os.close(r);server.terminate();server.wait(timeout=3)
    def test_portal_build_and_missing_bus(self):
        if subprocess.run(['pkg-config','--exists','gio-2.0']).returncode:
            self.skipTest('GIO development files required')
        import shlex
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','gio-2.0','x11'],text=True))
        binary=pathlib.Path(self.tmp.name)/'helper-portal'
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-DKAWARI_PORTAL',str(ROOT/'native/global_shortcuts.cpp'),'-o',str(binary),*flags],check=True)
        env=dict(os.environ,XDG_SESSION_TYPE='wayland',DBUS_SESSION_BUS_ADDRESS='unix:path='+str(pathlib.Path(self.tmp.name)/'missing-bus'))
        result=subprocess.run([binary,'test',chord(53),'Test'],env=env,capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,1)
        self.assertIn('STATUS ',result.stdout)
        self.assertNotIn('X11:',result.stdout)

    def test_wayland_never_falls_back_to_xwayland(self):
        result=subprocess.run([self.bin,'test',chord(53),'Test'],env=dict(os.environ,XDG_SESSION_TYPE='wayland'),capture_output=True,text=True,timeout=3)
        self.assertEqual(result.returncode,1);self.assertIn('Wayland portal support not built',result.stdout)

if __name__=='__main__':unittest.main()
