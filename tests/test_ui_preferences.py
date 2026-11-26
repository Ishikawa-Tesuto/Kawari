from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Preferences(unittest.TestCase):
    def test_preferences_and_geometry(self):
        source=r'''
#include "studio_ui_preferences.hpp"
#include "studio_ui_layout.hpp"
#include <cassert>
using namespace studio;
int main(int argc,char** argv){
    assert(argc==2);std::string path=std::string(argv[1])+"/ui.conf";
    ui::Preferences p,q;p.scale=130;p.russian=true;p.stream=true;p.fill=true;
    p.tools[0].open=true;p.tools[0].width=900;p.tools[1].open=true;p.tools[1].floating=true;
    p.tools[1].x=1700;p.tools[1].y=900;p.tools[1].w=620;p.tools[1].h=700;
    assert(ui::valid(p));auto s=ui::encode_preferences(p);
    assert(ui::decode_preferences(s,q)&&ui::encode_preferences(q)==s);
    assert(!ui::decode_preferences(s+" bad",q)&&ui::encode_preferences(q)==s);
    q.tools[1].floating=false;assert(!ui::valid(q));q=p;q.scale=999;assert(!ui::valid(q));
    assert(ui::write_preferences(path,p));assert(ui::read_preferences(path,q));assert(ui::encode_preferences(q)==s);
    struct stat st{};assert(stat(path.c_str(),&st)==0&&(st.st_mode&0777)==0600);
    assert(chmod(path.c_str(),0644)==0);assert(!ui::read_preferences(path,q));assert(!ui::write_preferences(path,p));
    assert(unlink(path.c_str())==0);assert(symlink("/dev/null",path.c_str())==0);assert(!ui::write_preferences(path,p));assert(!ui::read_preferences(path,q));
    ui::preferences()=p;assert(ui::window_label("Physics")=="Физика###Physics");ui::preferences().russian=false;assert(ui::window_label("Physics")=="Physics###Physics");
    auto t=p.tools[1];ui::clamp_float(t,800,600,36);assert(t.x+t.w<=800&&t.y+t.h<=600&&t.h<=516);
    for(float scale:{.85f,1.f,1.15f,1.3f,1.5f})for(float width:{320.f,520.f,3000.f}){
        auto a=ui_workspace_layout(1280,720,false,width,scale);assert(a.right>=320&&1280-a.left-a.right>=300&&a.bar>0);
        auto b=ui_workspace_layout(800,600,false,width,scale);assert(b.right==0);
    }
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test'),d],check=True)
    def test_integration(self):
        s=(ROOT/'native/studio_ui.cpp').read_text();theme=(ROOT/'native/studio_ui_theme.hpp').read_text()
        self.assertIn('resize_dock(dock,sz)',s);self.assertIn('persist_workspace_preferences(true)',s)
        self.assertIn('ui::navigation_icon(i+1',s);self.assertIn('s.ScaleAllSizes(scale())',theme)
        for f in ['scripts/build.sh','scripts/patch_sdk.py','scripts/source-id.sh']:
            self.assertIn('studio_ui_preferences.hpp',(ROOT/f).read_text())
if __name__=='__main__':unittest.main()
