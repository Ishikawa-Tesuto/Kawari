import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Background(unittest.TestCase):
    def test_cpp_background_state(self):
        source=r'''
#include "studio_background.hpp"
#include <cassert>
int main(){
    using namespace studio;
    unsetenv("STUDIO_NO_PREVIEW"); assert(!no_preview_requested());
    setenv("STUDIO_NO_PREVIEW","1",1); assert(no_preview_requested());
    assert(can_preview(false,true,false,800,600));
    assert(!can_preview(true,true,false,800,600));
    assert(!can_preview(false,false,false,800,600));
    assert(!can_preview(false,true,true,800,600));
    assert(!can_preview(false,true,false,0,600));
    assert(!can_preview(false,true,false,800,0));
    assert(!background_frame()); set_background_frame(true); assert(background_frame());
    set_background_frame(false); assert(!background_frame());
    install_stop_signals(); assert(!stop_requested());
    std::raise(SIGINT); assert(stop_requested());
    install_stop_signals(); assert(!stop_requested());
    std::raise(SIGTERM); assert(stop_requested());
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp); (p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_build_tracks_background_header(self):
        for file in ['scripts/build.sh','scripts/source-id.sh','scripts/patch_sdk.py']:
            self.assertIn('studio_background.hpp',(ROOT/file).read_text())
    def test_launcher_sets_environment(self):
        s=(ROOT/'legacy/viewer.rs').read_text()
        self.assertIn('opts.no_preview=true; opts.broadcast=true; opts.offscreen=true;',s)
        self.assertIn('.env("STUDIO_NO_PREVIEW",if opts.no_preview {"1"} else {"0"})',s)
if __name__=='__main__': unittest.main()
