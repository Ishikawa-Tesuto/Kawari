import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Workspace(unittest.TestCase):
    def test_preview_geometry(self):
        source=r'''
#include "studio_render_size.hpp"
#include <cassert>
#include <cmath>
using namespace studio;
int main(){
    assert(ui_parameter_matches("ParamEyeBallX", "Eyeball"));
    assert(ui_parameter_matches(u8"АКУЛИЙ ХВОСТ", u8"акулий хвост"));
    assert(ui_parameter_matches(u8"Рожки и Ёлка", u8"РОЖКИ И ёЛКА"));
    assert(ui_parameter_matches(u8"ТесутоXXI", u8"тЕСУТОxxi"));
    assert(!ui_parameter_matches(u8"Хвост", u8"Уши"));
    assert(ui_search_fold(u8"АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ")==u8"абвгдеёжзийклмнопрстуфхцчшщъыьэюя");
    assert(ui_parameter_matches("ParamEyeBallY", "EyeBall"));
    assert(ui_parameter_matches("ParamEyeBallX", "EYEBALL"));
    assert(ui_parameter_matches("ParamEyeBallY", "eyeball"));
    assert(ui_parameter_matches("ParamEyeBallX", "parameyeballx"));
    assert(ui_parameter_matches("ParamEyeBallX", ""));
    assert(!ui_parameter_matches("ParamEyeBallX", "eyebally"));
    assert(!ui_parameter_matches("ParamMouthOpenY", "eyeball"));
    assert(!ui_parameter_matches("", "eyeball"));
    setenv("STUDIO_UI","1",1);setenv("STUDIO_BROADCAST","1",1);unsetenv("STUDIO_NO_PREVIEW");
    for(int w: {320,800,1280,1920}) for(int h: {320,600,720,1080}) {
        const auto r=preview_rect(w,h,{1920,1080});
        assert(r.y>=ui_bar_height(h)-.001 && r.y+r.h<=h-ui_bar_height(h)+.001);
        assert(std::abs(r.w/r.h-1920.f/1080)<.001);
        assert(r.x>=-.001 && r.x+r.w<=w+.001);
        float x,y;assert(preview_point(w*.5f,h*.5f,w,h,{1920,1080},true,x,y));assert(std::abs(x)<.001&&std::abs(y)<.001);
        assert(!preview_point(w*.5f,5,w,h,{1920,1080},true,x,y));
    }
    ui_fill_preview()=true;
    for (int w : {320,800,1280,1920}) for (int h : {320,600,720,1080}) for (bool compact : {false,true}) {
        ui_compact_bars()=compact;
        for (auto canvas : {CanvasSize{1920,1080},CanvasSize{1080,1920}}) {
            auto r=preview_rect(w,h,canvas);
            assert(r.w>=w-.001 && r.h>=ui_preview_height(h)-.001);
            assert(std::abs(r.w/r.h-float(canvas.width)/canvas.height)<.001);
            assert(preview_contains(w*.5f,h*.5f,w,h,r));
            assert(!preview_contains(w*.5f,1,w,h,r));
            float x,y;assert(preview_point(w*.5f,h*.5f,w,h,canvas,true,x,y));assert(std::abs(x)<.001 && std::abs(y)<.001);
        }
        assert(output_size(w,h).width==1920 && output_size(w,h).height==1080);
    }
    ui_fill_preview()=false;ui_compact_bars()=false;
    assert(render_size(800,600).width==1920 && render_size(800,600).height==1080);
    setenv("STUDIO_NO_PREVIEW","1",1); assert(!ui_requested());
    unsetenv("STUDIO_NO_PREVIEW");setenv("STUDIO_UI","0",1);assert(ui_preview_height(600)==600);
    setenv("STUDIO_UI","1",1);setenv("STUDIO_BROADCAST","0",1);assert(!ui_requested());
    setenv("STUDIO_BROADCAST","1",1);ui_available()=false;assert(ui_preview_height(600)==600);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_docked_workspace_geometry(self):
        source=r'''
#include "studio_render_size.hpp"
#include <cassert>
#include <cmath>
using namespace studio;
bool near(float a,float b){return std::abs(a-b)<.01f;}
int main(){
    setenv("STUDIO_UI","1",1);setenv("STUDIO_BROADCAST","1",1);unsetenv("STUDIO_NO_PREVIEW");
    for(int w:{320,800,960,1280,1920})for(int h:{300,420,720,1080})for(bool compact:{false,true})for(float dock:{0.f,520.f,720.f})for(float scale:{.85f,1.f,1.5f}){
        const auto closed=ui_workspace_layout(w,h,compact,0,scale);
        const auto state=ui_workspace_layout(w,h,compact,dock,scale);
        ui_workspace()=state;
        const auto a=ui_preview_area(w,h);
        assert(a.w>0&&a.h>0);
        if(compact)assert(state.left==0&&state.right==0);
        else assert(state.left>0);
        if(w<960||h<420)assert(state.right==0);
        if(dock>0&&!compact&&w>=960&&h>=420)assert(state.right>0&&a.input_right-a.x>=299.99f);
        auto twice=ui_preview_area(w*2,h*2);assert(near(twice.x,2*a.x)&&near(twice.y,2*a.y)&&near(twice.w,2*a.w)&&near(twice.h,2*a.h));
        assert(near(twice.input_right,2*a.input_right));
        for(bool fill:{false,true})for(auto canvas:{CanvasSize{1920,1080},CanvasSize{1080,1920}}){
            ui_fill_preview()=fill;
            const auto r=preview_rect(w,h,canvas);const auto rr=preview_rect(w*2,h*2,canvas);
            assert(near(r.x+r.w*.5f,a.x+a.w*.5f)&&near(r.y+r.h*.5f,a.y+a.h*.5f));
            assert(near(rr.x,r.x*2)&&near(rr.y,r.y*2)&&near(rr.w,r.w*2)&&near(rr.h,r.h*2));
            if(fill)assert(r.w>=a.w-.01f&&r.h>=a.h-.01f);
            else assert(r.w<=a.w+.01f&&r.h<=a.h+.01f);
            const float y_screen=a.y+a.h*.5f;
            const float visible_left=std::max(a.x,r.x),visible_right=std::min(a.input_right,r.x+r.w);
            const float covered_left=std::max(a.input_right,r.x),covered_right=std::min(float(w),r.x+r.w);
            const float visible_x=(visible_left+visible_right)*.5f;
            const float covered_x=(covered_left+covered_right)*.5f;
            float closed_x,closed_y,x,y;
            ui_workspace()=closed;
            const auto before=preview_rect(w,h,canvas);
            assert(near(r.x,before.x)&&near(r.y,before.y)&&near(r.w,before.w)&&near(r.h,before.h));
            if(visible_right>visible_left)assert(preview_point(visible_x,y_screen,w,h,canvas,true,closed_x,closed_y));
            if(state.right>0&&covered_right>covered_left)assert(preview_contains(covered_x,y_screen,w,h,before));
            ui_workspace()=state;
            if(visible_right>visible_left){
                assert(preview_point(visible_x,y_screen,w,h,canvas,true,x,y));
                assert(near(x,closed_x)&&near(y,closed_y));
            }
            if(state.right>0&&covered_right>covered_left)
                assert(!preview_point(covered_x,y_screen,w,h,canvas,true,x,y));
            if(state.left>0)assert(!preview_contains(a.x*.5f,h*.5f,w,h,r));
            if(state.right>0)assert(!preview_contains(w-state.right*.5f,h*.5f,w,h,r));
            assert(!preview_contains(a.x+a.w*.5f,1,w,h,r));
            assert(output_size(w,h).width==1920&&output_size(w,h).height==1080);
        }
    }
    setenv("STUDIO_NO_PREVIEW","1",1);auto a=ui_preview_area(1280,720);assert(a.x==0&&a.y==0&&a.w==1280&&a.h==720&&a.input_right==1280);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_shortcut_format(self):
        source=r'''
#include "studio_ui_layout.hpp"
#include <cassert>
using namespace studio;
int main(){
    unsigned v=0;
    assert(shortcut_normalize(0,v)&&v==0);
    assert(shortcut_normalize(17,v)&&v==shortcut_encode(294,2));
    assert(shortcut_label(17)=="Ctrl + F5");
    assert(shortcut_label(shortcut_encode(85,3))=="Ctrl + Shift + U");
    assert(shortcut_encode(85,3)!=shortcut_encode(85,2));
    assert(shortcut_label(shortcut_encode(325,0))=="NumPad 5");
    assert(shortcut_label(shortcut_encode(334,4))=="Alt + NumPad +");
    assert(shortcut_encode(325,0)!=shortcut_encode(53,0));
    assert(shortcut_label(shortcut_encode(53,0))=="5");
    assert(shortcut_label(shortcut_encode(334,8))=="Super + NumPad +");
    assert(shortcut_label(shortcut_encode(314,0))=="F25");
    assert(shortcut_encode(256,0)==0&&shortcut_encode(256,2)!=0);
    assert(shortcut_encode(340,0)==0&&shortcut_encode(999,0)==0);
    assert(!shortcut_normalize(127,v)&&!shortcut_normalize(0xffffffffu,v));
    for(unsigned k=0;k<512;++k)for(unsigned m=0;m<16;++m){
        const auto c=shortcut_encode(k,m);if(c)assert(shortcut_normalize(c,v)&&v==c);
    }
    for(unsigned m=0;m<8;++m)for(unsigned k=1;k<=8;++k){
        assert(shortcut_normalize((m<<4)|k,v));
        assert((v&511u)==293+k);
    }
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_api_input_catalog_and_range_meter(self):
        source=r'''
#include "studio_ui_layout.hpp"
#include <cassert>
using namespace studio;
int main(){
    std::vector<UiInput> rows;
    std::istringstream good("kawari-inputs-1\nI\tBodyPositionX\tplugin\tVBridger\t-1\t1\t0\t0.25\t1\t0\nI\tFaceAngleX\tbuiltin\tVTube Studio\t-30\t30\t0\t0\t0\t1\n");
    assert(ui_parse_inputs(good,rows)&&rows.size()==2);
    assert(rows[0].name=="BodyPositionX"&&rows[0].owner=="VBridger"&&rows[0].live&&!rows[0].routed&&rows[0].value==.25f);
    assert(!rows[1].live&&rows[1].routed);
    assert(ui_range_position(0,-1,1)==.5f);
    assert(ui_range_position(.5f,1,-1)==.25f);
    assert(ui_range_position(2,-1,1)==1.f);
    assert(ui_range_position(-2,-1,1)==0.f);
    assert(ui_range_position(0,0,0)==0.f);
    std::istringstream bad("kawari-inputs-1\nI\tBodyPositionX\tplugin\tVBridger\t-1\t1\t0\tnan\t1\t0\n");
    assert(!ui_parse_inputs(bad,rows)&&rows.size()==2);
    std::istringstream empty("kawari-inputs-1\n");
    assert(ui_parse_inputs(empty,rows)&&rows.empty());
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_parameter_search_uses_case_insensitive_filter(self):
        s=(ROOT/'native/studio_ui.cpp').read_text()
        self.assertIn('ui_parameter_matches(d.id,parameter_filter)',s)
        self.assertNotIn('d.id.find(parameter_filter)',s)
    def test_wsi_only(self):
        s=(ROOT/'scripts/patch_sdk.py').read_text()
        self.assertIn('if (!studio::background_frame())',s)
        self.assertIn('if (studio::background_frame()) return;',s)
        self.assertIn('studio::ui_draw(commandBuffer);',s)
        self.assertIn('studio::ui_shutdown();',s)
        for f in ('studio_ui.cpp','studio_ui.hpp','studio_ui_layout.hpp'):
            for p in ('scripts/patch_sdk.py','scripts/build.sh','scripts/source-id.sh'):
                self.assertIn(f,(ROOT/p).read_text())
if __name__=='__main__': unittest.main()
