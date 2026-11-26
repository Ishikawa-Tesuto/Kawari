#include "studio_animations.hpp"
#include <cassert>
#include <fstream>
using namespace studio::animation;
static std::string model="/models/test.model3.json", path;
namespace studio {
const std::string& active_original_model(){return model;}
std::string animation_profile_path(){return path;}
}
int main(){
    char dir[]="/tmp/kawari-animation-XXXXXX";assert(mkdtemp(dir));path=std::string(dir)+"/settings";
    {
        Editor e;e.rows.emplace_back("Idle",0,"idle.motion3.json",3,true,false);e.rows.emplace_back("Other",0,"wave.motion3.json",2,false,false);e.rows.emplace_back("Voice",0,"voice.motion3.json",5,false,true);
        e.activate();assert(active()==&e);assert(!e.dirty());assert(e.next(true).motion==model_idle);assert(e.next(false).motion==keep);
        e.started_default("Idle",0,true);assert(e.playing()==0);
        e.play(1);auto p=e.next(false);assert(p.motion==1&&p.restart);e.started(1,true);e.current.speed=1.5f;assert(e.speed()==1.5f);
        e.repeat_preview=true;assert(e.next(true).motion==1);e.repeat_preview=false;p=e.next(true);assert(p.motion==model_idle&&p.restart&&e.preview()==no_idle);
        e.play(2);p=e.next(false);assert(p.motion==2);e.started(2,true);assert(e.speed()==1.f);
        e.idle(0);p=e.next(false);assert(p.motion==0&&p.restart&&e.preview()==no_idle);e.started(0,true);assert(e.next(false).motion==keep);
        assert(e.dirty()&&e.store()&&!e.dirty());struct stat st{};assert(stat(path.c_str(),&st)==0&&(st.st_mode&0777)==0600);
        e.idle(no_idle);assert(e.next(false).motion==no_idle);e.revert();assert(e.next(false).motion==0);
        e.play(1);assert(e.next(false).motion==1);e.started(1,false);assert(e.next(true).motion==keep);e.stop();assert(e.next(true).motion==0);
    }assert(active()==nullptr);
    {
        Editor e;e.rows.emplace_back("Other",0,"wave.motion3.json",2,false,false);e.rows.emplace_back("Idle",0,"idle.motion3.json",3,true,false);e.activate();assert(e.current.idle==1&&e.current.speed==1.5f);assert(e.next(true).motion==1);
        model="/models/different.model3.json";Editor other;other.rows=e.rows;other.activate();assert(other.current.idle==model_idle&&other.current.speed==1.f&&!other.status.empty());
        assert(!parse("kawari-animations-1\n/models/different.model3.json\n99\ndefault\n",model,other.rows,other.current));
    }
    unlink(path.c_str());const auto target=std::string(dir)+"/target";{std::ofstream f(target);f<<"original";}assert(symlink(target.c_str(),path.c_str())==0);
    {Editor e;e.activate();assert(!e.status.empty());assert(!e.store());}
    {std::ifstream f(target);std::string x;f>>x;assert(x=="original");}
    unlink(path.c_str());unlink(target.c_str());rmdir(dir);
}
