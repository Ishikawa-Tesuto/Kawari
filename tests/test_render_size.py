import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class MasterCanvas(unittest.TestCase):
    def test_cpp_geometry_and_environment(self):
        source=r'''
#include "studio_render_size.hpp"
#include <cassert>
#include <cmath>
using namespace studio;
bool near(float a,float b){return std::fabs(a-b)<0.001f;}
int main(){
    unsetenv("STUDIO_BROADCAST");
    assert(render_size(400,300).width==400 && render_size(400,300).height==300);
    setenv("STUDIO_BROADCAST","1",1);
    unsetenv("STUDIO_BROADCAST_WIDTH"); unsetenv("STUDIO_BROADCAST_HEIGHT");
    assert(render_size(400,300).width==1920 && render_size(400,300).height==1080);
    assert(render_size(0,0).width==1920);
    auto a=preview_rect(800,600,render_size(800,600));
    assert(near(a.w,800) && near(a.h,450) && near(a.y,75));
    float x=0,y=0;
    assert(!preview_point(400,25,800,600,{1920,1080},true,x,y));
    assert(preview_point(400,300,800,600,{1920,1080},true,x,y) && near(x,0) && near(y,0));
    assert(preview_point(800,300,800,600,{1920,1080},true,x,y) && near(x,1920.f/1080));
    assert(preview_point(400,75,800,600,{1920,1080},true,x,y) && near(y,1));
    auto b=preview_rect(1200,500,{1080,1920});
    assert(near(b.w,281.25f) && near(b.h,500) && near(b.x,459.375f));
    assert(preview_point(600,0,1200,500,{1080,1920},true,x,y) && near(y,1920.f/1080));
    assert(preview_point(600,0,1200,500,{1080,1920},false,x,y) && near(y,1));
    assert(!preview_point(0,0,0,0,{1920,1080},true,x,y));
    setenv("STUDIO_BROADCAST_WIDTH","2560",1);setenv("STUDIO_BROADCAST_HEIGHT","1440",1);
    int w=360,h=640; apply_render_size(w,h);assert(w==2560 && h==1440);
    w=2000;h=900;apply_render_size(w,h);assert(w==2560 && h==1440);
    setenv("STUDIO_BROADCAST_WIDTH","garbage",1);setenv("STUDIO_BROADCAST_HEIGHT","99999",1);
    assert(render_size(1,1).width==1920 && render_size(1,1).height==1080);
    setenv("STUDIO_SSAA","2",1);
    assert(render_size(400,300).width==3840 && render_size(400,300).height==2160);
    assert(output_size(400,300).width==1920 && output_size(400,300).height==1080);
    setenv("STUDIO_BROADCAST_WIDTH","7680",1);assert(render_size(400,300).width==7680);
    setenv("STUDIO_BROADCAST","0",1);assert(render_size(400,300).width==400);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=pathlib.Path(tmp); (p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_build_copies_and_hashes_header(self):
        for f in ['scripts/build.sh','scripts/source-id.sh','scripts/patch_sdk.py']:
            self.assertIn('studio_render_size.hpp',(ROOT/f).read_text())
if __name__=='__main__': unittest.main()
