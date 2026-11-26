#pragma once
#include "studio_physics_settings.hpp" // private profile file checks
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
namespace studio {
const std::string& active_original_model();
std::string animation_profile_path();
namespace animation {
// Row identities are checked against the current manifest on every profile load.
struct Motion {
    std::string group,file;
    int number;
    float duration;
    bool loop,sound;
    Motion(std::string g,int n,std::string f,float d,bool l,bool s):group(std::move(g)),file(std::move(f)),number(n),duration(d),loop(l),sound(s){}
};
constexpr int model_idle=-1, no_idle=-2, keep=-3;
struct Settings {int idle=model_idle;float speed=1.f;};
inline bool valid_speed(float v){return std::isfinite(v)&&v>=.25f&&v<=2.f;}
inline bool same(Settings a,Settings b){return a.idle==b.idle&&a.speed==b.speed;}
struct Plan {int motion;bool restart;Plan(int m=keep,bool r=false):motion(m),restart(r){}};
inline bool valid_index(int i,const std::vector<Motion>& rows){return i>=0&&size_t(i)<rows.size();}
inline bool serialize(const std::string& model,const std::vector<Motion>& rows,Settings settings,std::string& result){
    if(!physics::safe_field(model)||!valid_speed(settings.speed))return false;
    std::ostringstream out;out.imbue(std::locale::classic());out.precision(std::numeric_limits<float>::max_digits10);
    out<<"kawari-animations-1\n"<<model<<'\n'<<settings.speed<<'\n';
    if(settings.idle==model_idle)out<<"default\n";
    else if(settings.idle==no_idle)out<<"none\n";
    else if(valid_index(settings.idle,rows)){
        const auto& m=rows[size_t(settings.idle)];if(!physics::safe_field(m.group)||!physics::safe_field(m.file))return false;
        out<<"motion\n"<<m.group<<'\n'<<m.number<<'\n'<<m.file<<'\n';
    }else return false;
    result=out.str();return true;
}
inline bool parse(const std::string& data,const std::string& model,const std::vector<Motion>& rows,Settings& settings){
    if(data.size()>32768)return false;
    std::istringstream in(data);in.imbue(std::locale::classic());std::string line,extra;Settings next;
    if(!std::getline(in,line)||line!="kawari-animations-1"||!std::getline(in,line)||line!=model||!std::getline(in,line))return false;
    std::istringstream value(line);value.imbue(std::locale::classic());if(!(value>>next.speed)||(value>>extra)||!valid_speed(next.speed))return false;
    if(!std::getline(in,line))return false;
    if(line=="none")next.idle=no_idle;
    else if(line=="motion"){
        std::string group,file;int number;
        if(!std::getline(in,group)||!std::getline(in,line)||!std::getline(in,file))return false;
        std::istringstream n(line);if(!(n>>number)||(n>>extra))return false;
        auto it=std::find_if(rows.begin(),rows.end(),[&](const Motion& m){return m.group==group&&m.number==number&&m.file==file;});
        if(it==rows.end())return false;
        next.idle=int(it-rows.begin());
    }else if(line!="default")return false;
    if(std::getline(in,extra))return false;
    settings=next;return true;
}
inline bool load(const std::string& path,const std::string& model,const std::vector<Motion>& rows,Settings& s,std::string& error){
    if(path.empty())return true;
    int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT)return true;error="Could not open animation profile safely.";return false;}
    struct stat st{};if(::fstat(fd,&st)!=0||!physics::safe_file(st)||st.st_size<0||st.st_size>32768){::close(fd);error="Unsafe animation profile.";return false;}
    std::string data;char buf[4096];ssize_t n;
    do {n=::read(fd,buf,sizeof(buf));if(n>0)data.append(buf,size_t(n));}while((n>0||(n<0&&errno==EINTR))&&data.size()<=32768);
    ::close(fd);if(n!=0||!parse(data,model,rows,s)){error="Invalid or outdated animation profile; model defaults retained.";return false;}return true;
}
inline bool save(const std::string& path,const std::string& model,const std::vector<Motion>& rows,Settings s,std::string& error){
    std::string data;if(path.empty()||!serialize(model,rows,s,data)){error="No writable animation profile.";return false;}
    struct stat st{};int found=::lstat(path.c_str(),&st);
    if((found==0&&!physics::safe_file(st))||(found!=0&&errno!=ENOENT)){error="Unsafe animation profile; not overwritten.";return false;}
    std::string pattern=path+".tmp-XXXXXX";std::vector<char> tmp(pattern.begin(),pattern.end());tmp.push_back(0);
    int fd=::mkstemp(tmp.data());if(fd<0){error="Could not create animation profile.";return false;}
    FILE* f=::fdopen(fd,"wb");bool ok=false;
    if(f){ok=std::fwrite(data.data(),1,data.size(),f)==data.size();if(std::fflush(f)!=0)ok=false;if(::fsync(fd)!=0)ok=false;if(std::fclose(f)!=0)ok=false;}else ::close(fd);
    if(ok)ok=::rename(tmp.data(),path.c_str())==0;
    if(!ok){::unlink(tmp.data());error="Could not save animation profile; previous file retained.";}
    return ok;
}
class Editor;
inline Editor*& active(){static Editor* e=nullptr;return e;}
class Editor {
    Settings saved_;
    bool loaded_=false,pending_=false,failed_=false;
    std::string model_,path_;
    int preview_=no_idle,playing_=no_idle;
public:
    std::vector<Motion> rows;
    Settings current;
    bool repeat_preview=false;
    std::string status;
    ~Editor(){if(active()==this)active()=nullptr;}
    Editor()=default;
    Editor(const Editor&)=delete;Editor& operator=(const Editor&)=delete;
    void activate(){
        active()=this;if(loaded_)return;loaded_=true;
        model_=active_original_model();path_=animation_profile_path();load(path_,model_,rows,current,status);saved_=current;
        pending_=current.idle!=model_idle;
    }
    bool dirty()const{return !same(current,saved_);}
    bool can_save()const{return !path_.empty();}
    int preview()const{return preview_;}
    int playing()const{return playing_;}
    void play(int i){if(!valid_index(i,rows))return;preview_=i;pending_=true;failed_=false;status.clear();}
    void stop(){preview_=no_idle;pending_=true;failed_=false;status.clear();}
    void idle(int i){if(i!=model_idle&&i!=no_idle&&!valid_index(i,rows))return;if(current.idle==i)return;current.idle=i;stop();}
    void revert(){current=saved_;stop();}
    void reset(){current=Settings{};stop();}
    bool store(){if(!save(path_,model_,rows,current,status))return false;saved_=current;status="Saved";return true;}
    Plan next(bool finished){
        if(pending_){pending_=false;failed_=false;return {preview_>=0?preview_:current.idle,true};}
        if(failed_||!finished)return {};
        if(preview_>=0){if(repeat_preview)return {preview_,false};preview_=no_idle;return {current.idle,true};}
        return {current.idle,false};
    }
    void started(int index,bool ok){playing_=ok?index:no_idle;failed_=!ok&&index>=0;if(failed_)status="Motion could not be started.";}
    // Resolve the actual chosen SDK Idle row without changing its random choice.
    void started_default(const char* group,int number,bool ok){
        if(!ok){started(model_idle,false);return;}
        auto it=std::find_if(rows.begin(),rows.end(),[&](const Motion& m){return m.group==group&&m.number==number;});
        started(it==rows.end()?model_idle:int(it-rows.begin()),true);
    }
    float speed()const{
        // Do not desynchronize the existing SDK audio/lip-sync path.
        return valid_index(playing_,rows)&&!rows[size_t(playing_)].sound&&valid_speed(current.speed)?current.speed:1.f;
    }
};
}
}
