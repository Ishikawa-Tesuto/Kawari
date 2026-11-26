#include "studio_physics_settings.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
using namespace studio::physics;
int main(){
    Settings a;a.groups.emplace_back("Hair","Hair swing");a.groups.emplace_back("Tail","Tail motion");
    a.enabled=false;a.strength=1.25f;a.fps=120;a.groups[0].factor=0.4f;a.groups[1].factor=2.f;
    const std::string model="/models/Model with spaces.model3.json";
    std::string data;assert(serialize(a,model,data));
    Settings b=a;b.reset();assert(!same(a,b));assert(parse(data,model,b)&&same(a,b));
    Settings reversed=b;std::swap(reversed.groups[0],reversed.groups[1]);reversed.reset();assert(parse(data,model,reversed));assert(reversed.groups[0].factor==2.f&&reversed.groups[1].factor==0.4f);
    auto bad=b;assert(!parse(data,"/other.model3.json",bad)&&same(bad,b));
    assert(!parse(data+"Hair\t1\n",model,bad)&&same(bad,b));
    assert(!parse(data+"Unknown\t1\n",model,bad)&&same(bad,b));
    assert(!parse("kawari-physics-1\n"+model+"\n1 3 60\nHair\t1\nTail\t1\n",model,bad));
    assert(!parse("kawari-physics-1\n"+model+"\n1 1 90\nHair\t1\nTail\t1\n",model,bad));
    assert(!parse("kawari-physics-1\n"+model+"\n1 nan 60\nHair\t1\nTail\t1\n",model,bad));
    assert(!parse("kawari-physics-1\n"+model+"\n1 1 60\nHair\t1\n",model,bad));
    assert(!valid_factor(std::numeric_limits<float>::infinity()));
    assert(!valid_factor(-1));assert(!valid_fps(144));
    char dir[]="/tmp/kawari-physics-XXXXXX";assert(mkdtemp(dir));
    std::string path=std::string(dir)+"/settings", error;
    assert(load(path,model,b,error));assert(save(path,model,a,error));
    struct stat st{};assert(lstat(path.c_str(),&st)==0&&(st.st_mode&0777)==0600);
    b.reset();assert(load(path,model,b,error)&&same(a,b));
    a.strength=0.75f;assert(save(path,model,a,error));assert(load(path,model,b,error)&&same(a,b));
    const auto link=std::string(dir)+"/link";assert(symlink(path.c_str(),link.c_str())==0);
    assert(!load(link,model,b,error));assert(!save(link,model,b,error));
    assert(chmod(path.c_str(),0644)==0);assert(!load(path,model,b,error));assert(!save(path,model,b,error));
    assert(chmod(path.c_str(),0600)==0);assert(load(path,model,b,error)&&same(a,b));
    assert(unlink(link.c_str())==0);assert(unlink(path.c_str())==0);assert(rmdir(dir)==0);
}
