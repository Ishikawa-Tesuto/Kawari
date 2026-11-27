"""Combined expression envelope and stable item identity, without a GPU."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class Combined(unittest.TestCase):
    def test_envelopes_and_base_scene(self):
        code=r'''
#include "studio_items.hpp"
#include <cassert>
using namespace studio;
int main(){
    parameters::Runtime r;r.reset({{"X",-10,10,0}});
    const auto t=parameters::Clock::now();
    auto at=[&](int ms){return t+std::chrono::milliseconds(ms);};
    items::Item i;i.path="/some/kawari/items/asset-aB12cD/очки.png";i.visible=false;i.opacity=.8f;
    const auto saved=items::encode({i});
    auto opacity=[&](int ms){return items::effective(i,r,at(ms)).opacity;};
    auto near=[](float x,float y){assert(std::abs(x-y)<.001f);};
    assert(items::asset_id(i)=="asset-aB12cD");
    assert(r.ingest("expr on2 combined 3000 0 500 X O 8 asset-aB12cD I 1",t));
    float v=0;r.apply(at(1500),.016f,[&](size_t){return v;},[&](size_t,float x){v=x;});near(v,4);near(opacity(1500),.4f);near(opacity(3000),.8f);
    assert(r.ingest("expr on2 hide 1000 0 500 asset-aB12cD I 0",at(3000)));
    near(opacity(3500),.4f);near(opacity(4000),0);
    assert(r.ingest("expr off hide",at(4000)));near(opacity(4250),.4f);near(opacity(4500),.8f);
    assert(r.ingest("expr off combined",at(4500)));near(opacity(4750),.4f);near(opacity(5000),0);
    assert(items::encode({i})==saved);
    assert(r.ingest("expr reset",at(5100)));
    assert(r.ingest("expr on2 itemonly 100 300 200 asset-aB12cD I 1",at(5100)));
    near(opacity(5150),.4f);near(opacity(5400),.8f);near(opacity(5500),.4f);near(opacity(5600),0);
    assert(r.ingest("expr on editor-preview 0 500 asset-aB12cD I 1",at(5700)));near(opacity(5700),.8f);
    r.clear_expression_preview();near(opacity(5700),0);
    assert(!r.ingest("expr on bad 0 0 asset-aB12cD I 2",t));
    assert(!r.ingest("expr on bad 0 0 asset-aB12cD I 0 asset-aB12cD I 1",t));
    assert(!r.ingest("expr on bad 0 0 ../asset I 1",t));
    assert(!r.ingest("expr on bad 0 0 Unknown O 1 asset-aB12cD I 1",t));
    items::Item other=i;other.path="/some/kawari/items/asset-abcdef/очки.png";
    assert(r.ingest("expr on itemonly 0 0 asset-aB12cD I 1",at(5800)));assert(!items::effective(other,r,at(5800)).visible);
    assert(r.ingest("expr reset",at(5900)));near(opacity(5900),0);
    i.visible=true;assert(r.ingest("expr on hide 0 0 asset-aB12cD I 0",at(6000)));near(opacity(6000),0);
    r.reset({{"New",0,1,0}});near(opacity(6000),.8f);
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_model_position_transitions(self):
        code=r'''
#include "studio_parameters.hpp"
#include <cassert>
using namespace studio::parameters;
int main(){
    Runtime runtime;
    runtime.reset({{"Smile",0,1,0}});
    PositionMotion motion;
    const auto time=Clock::now();
    const auto later=time+std::chrono::milliseconds(200);
    auto near=[](float a,float b){assert(std::abs(a-b)<.001f);};

    assert(runtime.ingest("expr on position-only 0 0 position P 0.6 0.8 1",time));
    motion.advance(.1f,{},runtime.model_position(time));
    near(motion.view.x,.06f);near(motion.view.y,.08f);
    assert(runtime.ingest("expr on2 timed 500 200 500 Smile O 1 position P 0.66 0.08 2",time));
    motion.advance(.1f,{},runtime.model_position(time));
    near(motion.view.x,.26f);near(motion.view.y,.08f);
    assert(runtime.model_position(later)->x==.6f);

    assert(runtime.ingest("expr on obs-position 0 0 position P 0.26 -0.42 1",later));
    motion.advance(.1f,{},runtime.model_position(later));
    near(motion.view.x,.26f);near(motion.view.y,-.02f);
    assert(runtime.ingest("expr off obs-position",later));
    assert(runtime.model_position(later)->x==.6f);
    assert(runtime.ingest("expr off position-only",later));
    assert(runtime.model_position(later)==nullptr);
    const float distance=std::hypot(motion.view.x,motion.view.y);
    motion.advance(.1f,{},runtime.model_position(later));
    near(std::hypot(motion.view.x,motion.view.y),distance-.1f);
    for(int i=0;i<50;++i)motion.advance(.01f,{},runtime.model_position(later));
    assert(!motion.active);near(motion.view.x,0);near(motion.view.y,0);

    assert(runtime.ingest("expr on obs-position 0 0 position V 0.4 0 1 2",later));
    const float travel=view_distance({},runtime.model_position(later)->target(1));
    motion.advance(travel*.1f,{},runtime.model_position(later));
    near(motion.view.x,.04f);near(motion.view.scale,std::pow(2.f,.1f));

    PositionMotion faster;
    const ModelView base(.2f,-.5f,1.5f);
    for(int i=0;i<200;++i)faster.advance(.01f,base,runtime.model_position(later));
    near(faster.view.x,.4f);near(faster.view.y,0);near(faster.view.scale,2.f);
    assert(runtime.ingest("expr off obs-position",later));
    faster.advance(.1f,base,runtime.model_position(later));
    assert(faster.view.scale<2.f&&faster.view.scale>1.5f);
    for(int i=0;i<100;++i)faster.advance(.01f,base,runtime.model_position(later));
    assert(!faster.active&&!faster.scaling&&faster.view==base);
    assert(runtime.ingest("expr on old 0 0 position P 0 0 1",later));
    faster.advance(.1f,base,runtime.model_position(later));
    near(faster.view.scale,1.5f);
    faster.advance(.1f,{.2f,-.5f,3.f},runtime.model_position(later));
    near(faster.view.scale,3.f);

    assert(!runtime.ingest("expr on bad 0 0 position V 0 0 1 0",time));
    assert(!runtime.ingest("expr on bad 0 0 position V 0 0 1 6",time));
    assert(!runtime.ingest("expr on bad 0 0 position P 0 0 0",time));
    assert(!runtime.ingest("expr on bad 0 0 position P 10001 0 1",time));
    assert(!runtime.ingest("expr on obs-position 0 0 Smile O 1 position P 0 0 1",time));
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_protocol_and_compositor(self):
        api=(ROOT/'api-rust/src/main.rs').read_text()
        editor=(ROOT/'api-rust/src/expression_editor.rs').read_text()
        ui=(ROOT/'native/studio_ui.cpp').read_text()
        vk=(ROOT/'native/studio_items_vk.cpp').read_text()
        self.assertIn('for(id,show)in &a.items',api)
        self.assertIn('"KawariItems":items',editor)
        self.assertIn('if(type=="I")',ui)
        self.assertIn('Unchanged\\0Show\\0Hide\\0',ui)
        self.assertIn('auto rows=s.rows;',vk)
        self.assertIn('i=effective(i,editor_runtime(),now)',vk)
        self.assertIn('for(auto& i:rows)if(!i.behind)',vk)

if __name__=='__main__':unittest.main()
