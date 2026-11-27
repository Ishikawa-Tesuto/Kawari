#include "studio_controls.hpp"
#include "studio_background.hpp"
#include "studio_ui_layout.hpp"
#include "studio_parameters.hpp"
#include "studio_parameter_socket.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <array>
#include <fstream>
#include <ctime>
#include <cmath>
#include <cstring>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

namespace studio {
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point frame_start;
int selected = 0;
float zoom = 1.f, pan_x = 0.f, pan_y = 0.f;
parameters::PositionMotion position_motion;
float zoom_target = 1.f, anchor_x = 0.f, anchor_y = 0.f;
Clock::time_point view_tick{};
float limit_pan(float v){return parameters::clamp(v,-parameters::pan_limit,parameters::pan_limit);}
float limit_zoom(float v){return parameters::clamp(v,parameters::min_scale,parameters::max_scale);}
void scale_about(float next,float x,float y){
    const float ratio=next/zoom;
    pan_x=limit_pan(x+(pan_x-x)*ratio);
    pan_y=limit_pan(y+(pan_y-y)*ratio);
    zoom=next;
}
int bg_index = 0;
constexpr float backgrounds[] = {0.025f, 0.18f, 0.65f, 0.f};
bool env_flag(const char* name) { const char* v=std::getenv(name); return v && std::strcmp(v,"1")==0; }
bool show_demo = env_flag("STUDIO_DEMO");
bool stats = env_flag("STUDIO_STATS");
int frame_limit=[] { const char* s=std::getenv("STUDIO_FPS"); const int n=s?std::atoi(s):60;return n==0||n==30||n==60||n==120||n==144?n:60; }();
Clock::time_point stats_start = Clock::now();
std::clock_t cpu_start = std::clock();
unsigned frames=0;
double work_ms=0, measured_fps=0;
int dimension(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (!value) return fallback;
    char* end = nullptr;
    const long n = std::strtol(value, &end, 10);
    return end != value && !*end && n >= 320 && n <= 7680 ? static_cast<int>(n) : fallback;
}
double rss_mib() {
    std::ifstream file("/proc/self/status");
    std::string line;
    while (std::getline(file,line)) if (line.compare(0,6,"VmRSS:")==0) {
        return std::strtod(line.c_str()+6,nullptr)/1024.0;
    }
    return -1;
}

std::string env_string(const char* key) { const char* p=std::getenv(key); return p?p:""; }

struct ViewProfile {
    std::string path;
    parameters::ModelView saved, desired;
    Clock::time_point next_save{};
    bool loaded=false, write_error=false;
};
ViewProfile view_profile;

bool safe_view_file(const struct stat& st){
    return S_ISREG(st.st_mode)&&st.st_uid==getuid()&&st.st_nlink==1&&!(st.st_mode&0077);
}
bool read_model_view(const std::string& path,parameters::ModelView& view){
    const int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return errno==ENOENT;
    struct stat st{};
    if(fstat(fd,&st)!=0||!safe_view_file(st)||st.st_size<=0||st.st_size>128){close(fd);return false;}
    std::string data;char buffer[129];ssize_t count;
    do{
        count=read(fd,buffer,sizeof(buffer));
        if(count>0)data.append(buffer,size_t(count));
    }while((count>0||(count<0&&errno==EINTR))&&data.size()<=128);
    close(fd);
    if(count!=0)return false;
    std::istringstream input(data);input.imbue(std::locale::classic());
    parameters::ModelView pending;std::string version,extra;
    if(!(input>>version>>pending.x>>pending.y>>pending.scale)||version!="kawari-view-1"||
       (input>>extra)||!pending.valid())return false;
    view=pending;return true;
}
bool write_model_view(const std::string& path,const parameters::ModelView& view){
    struct stat st{};
    const int result=lstat(path.c_str(),&st);
    if((result==0&&!safe_view_file(st))||(result!=0&&errno!=ENOENT))return false;
    std::ostringstream out;out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<float>::max_digits10);
    out<<"kawari-view-1\n"<<view.x<<' '<<view.y<<' '<<view.scale<<'\n';
    const auto data=out.str();
    const auto pattern=path+".tmp-XXXXXX";
    std::vector<char> tmp(pattern.begin(),pattern.end());tmp.push_back(0);
    const int fd=mkstemp(tmp.data());
    if(fd<0)return false;
    FILE* file=fdopen(fd,"wb");
    bool written=false;
    if(file){
        written=fwrite(data.data(),1,data.size(),file)==data.size();
        if(fflush(file)!=0||fsync(fd)!=0)written=false;
        if(fclose(file)!=0)written=false;
    }else close(fd);
    if(written&&std::rename(tmp.data(),path.c_str())==0)return true;
    unlink(tmp.data());return false;
}
void load_model_view(const std::string& path){
    view_profile=ViewProfile{};
    view_profile.path=path;view_profile.loaded=true;
    if(!path.empty()&&!read_model_view(path,view_profile.saved))
        std::fprintf(stderr,"[studio] Could not load model position and scale: %s\n",path.c_str());
    const auto& view=view_profile.saved;
    view_profile.desired=view;
    pan_x=view.x;pan_y=view.y;zoom=zoom_target=view.scale;
    anchor_x=anchor_y=0;view_tick={};position_motion={};
}
void save_model_view_if_due(Clock::time_point now){
    if(now>=view_profile.next_save)camera_save();
}
void remember_model_view(){
    if(view_profile.path.empty())return;
    // Save the requested endpoint even if the smooth wheel zoom is still animating.
    const float ratio=zoom_target/zoom;
    view_profile.desired={
        limit_pan(anchor_x+(pan_x-anchor_x)*ratio),
        limit_pan(anchor_y+(pan_y-anchor_y)*ratio),
        zoom_target,
    };
    save_model_view_if_due(Clock::now());
}
bool selection_committed=false;
ModelSelection& active_selection(){
    static ModelSelection s;
    if(!selection_committed){s.original=env_string("STUDIO_ORIGINAL_MODEL");s.runtime=env_string("STUDIO_MODEL");s.mapping=env_string("STUDIO_PARAMETER_MAP");s.view=env_string("STUDIO_VIEW_PROFILE");s.editable=!s.mapping.empty()&&env_string("STUDIO_EDITABLE_MAP")==s.mapping;}
    return s;
}
bool initialized = false;
parameters::Runtime parameter_runtime;
parameters::Socket parameter_socket;
Clock::time_point parameter_tick, snapshot_tick;
unsigned invalid_packets=0;
std::vector<float> editor_current;
int editor_target=-1;
float editor_test_value=0;
bool editor_changed=false, editor_map_valid=false;
std::unordered_map<int, float> overrides;
constexpr std::array<int, 7> keys{{GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT, GLFW_KEY_R, GLFW_KEY_P, GLFW_KEY_ESCAPE}};
std::array<bool, keys.size()> previous{};
using Model = Live2D::Cubism::Framework::CubismModel;
const char* id(Model* m, int i) { return m->GetParameterId(i)->GetString().GetRawString(); }
void list(Model* m) {
    std::fprintf(stdout, "\n[studio] %d parameters (index | ID | min | default | max)\n", m->GetParameterCount());
    for (int i = 0; i < m->GetParameterCount(); ++i)
        std::fprintf(stdout, "%4d | %s | %.4f | %.4f | %.4f\n", i, id(m,i), m->GetParameterMinimumValue(i), m->GetParameterDefaultValue(i), m->GetParameterMaximumValue(i));
    std::fflush(stdout);
}
// Atomic, private snapshot of actual model values for the local API bridge.
void publish_parameters(Model* m) {
    if(parameter_socket.path().empty()) return;
    const auto now=Clock::now();
    if(now-snapshot_tick<std::chrono::milliseconds(200)) return;
    snapshot_tick=now;
    const std::string base=parameter_socket.path().substr(0,parameter_socket.path().rfind('/'));
    const std::string tmp=base+"/parameters.tmp", dest=base+"/parameters.tsv";
    std::ofstream out(tmp,std::ios::trunc); out.imbue(std::locale::classic());
    out.precision(9);
    for(int i=0;i<m->GetParameterCount();++i) {
        out<<"P\t"<<id(m,i)<<'\t'<<m->GetParameterMinimumValue(i)<<'\t'<<m->GetParameterMaximumValue(i)<<'\t'<<m->GetParameterDefaultValue(i)<<'\t'<<m->GetParameterValue(i)<<'\n';
    }
    for(const auto& mapping:parameter_runtime.mappings()) {
        out<<"M\t"<<mapping.input<<'\t'<<id(m,static_cast<int>(mapping.target))<<'\t'<<mapping.in_min<<'\t'<<mapping.in_max<<'\t'<<mapping.out_min<<'\t'<<mapping.out_max<<'\n';
    }
    out.close();
    if(out) std::rename(tmp.c_str(),dest.c_str());
}
void title(Model* m, GLFWwindow* w) {
    char text[512];
    std::snprintf(text, sizeof(text), "Kawari | Vulkan | %d/%d %s: %.3f%s", selected+1, m->GetParameterCount(), id(m,selected), m->GetParameterValue(selected), overrides.count(selected)?" [manual]":" [auto]");
    glfwSetWindowTitle(w, text);
}
}
bool offscreen() { static const bool value=env_flag("STUDIO_OFFSCREEN"); return value; }
bool demo_visible() { return show_demo; }
float background() { return show_demo ? 0.f : backgrounds[bg_index]; }
int window_width(int fallback) { return dimension("STUDIO_WIDTH",fallback); }
int window_height(int fallback) { return dimension("STUDIO_HEIGHT",fallback); }
bool camera_is_default() {
    float x,y;camera_offset(x,y);
    return camera_scale()==1.f && zoom_target==1.f && x==0.f && y==0.f;
}
float camera_scale() { return position_motion.scaling?position_motion.view.scale:zoom; }
float camera_base_scale() { return zoom; }
void camera_offset(float& x,float& y){
    x=position_motion.active?position_motion.view.x:pan_x;
    y=position_motion.active?position_motion.view.y:pan_y;
}
void camera_zoom(float factor) {
    if(!std::isfinite(factor)||factor<=0)return;
    zoom=zoom_target=limit_zoom(zoom*factor);
    remember_model_view();
}
void camera_zoom_at(float factor,float x,float y) {
    if (!std::isfinite(factor) || factor<=0 || !std::isfinite(x) || !std::isfinite(y)) return;
    zoom_target=limit_zoom(zoom*factor);scale_about(zoom_target,x,y);
    remember_model_view();
}
void camera_scroll(float delta,float x,float y){
    if(!std::isfinite(delta)||delta==0||!std::isfinite(x)||!std::isfinite(y))return;
    // Fractional wheel/trackpad deltas accumulate in logarithmic scale.
    const float step=std::max(-64.f,std::min(64.f,delta));
    zoom_target=limit_zoom(zoom_target*std::pow(1.075f,step));
    anchor_x=x;anchor_y=y;
    remember_model_view();
}
void camera_advance(float dt){
    if(!std::isfinite(dt)||dt<=0||zoom==zoom_target)return;
    const float gap=std::log(zoom_target/zoom);
    const float alpha=-std::expm1(-std::min(dt,.1f)/.065f);
    const float next=std::abs(gap)<.00015f?zoom_target:zoom*std::exp(gap*alpha);
    scale_about(next,anchor_x,anchor_y);
}
void camera_pan(float x,float y) {
    if(!std::isfinite(x)||!std::isfinite(y)||(x==0.f&&y==0.f))return;
    const float old_x=pan_x,old_y=pan_y;
    pan_x=limit_pan(pan_x+x);pan_y=limit_pan(pan_y+y);
    // A drag during a zoom translates its anchor as well.
    anchor_x+=pan_x-old_x;anchor_y+=pan_y-old_y;
    remember_model_view();
}
void camera_reset(){
    zoom=zoom_target=1;pan_x=pan_y=anchor_x=anchor_y=0;view_tick={};
    remember_model_view();
}
void camera_save(){
    auto& profile=view_profile;
    if(profile.path.empty()||profile.desired==profile.saved)return;
    if(write_model_view(profile.path,profile.desired)){
        profile.saved=profile.desired;
        profile.write_error=false;
        profile.next_save=Clock::now()+std::chrono::milliseconds(250);
    }else{
        if(!profile.write_error)
            std::fprintf(stderr,"[studio] Could not save model position and scale: %s\n",profile.path.c_str());
        profile.write_error=true;
        profile.next_save=Clock::now()+std::chrono::seconds(2);
    }
}
void cycle_background() { show_demo=false;bg_index=(bg_index+1)%4; }
void apply_camera(float* m) {
    float x,y;camera_offset(x,y);
    const float scale=camera_scale();
    // Clip-space affine transform: scale XY, then translate by W.
    for (int i=0;i<16;i+=4) { m[i]=scale*m[i]+x*m[i+3]; m[i+1]=scale*m[i+1]+y*m[i+3]; }
}
void update_view(GLFWwindow* w) {
    if(!view_profile.loaded)load_model_view(active_selection().view);
    const auto now=Clock::now();
    const float dt=view_tick==Clock::time_point{}?0.f:std::chrono::duration<float>(now-view_tick).count();
    camera_advance(dt);
    position_motion.advance(dt,{pan_x,pan_y,zoom},parameter_runtime.model_position(now));
    save_model_view_if_due(now);
    view_tick=now;
    constexpr int actions[]={GLFW_KEY_EQUAL,GLFW_KEY_MINUS,GLFW_KEY_HOME,GLFW_KEY_B,GLFW_KEY_F2,GLFW_KEY_F3,GLFW_KEY_W,GLFW_KEY_A,GLFW_KEY_S,GLFW_KEY_D,GLFW_KEY_ESCAPE};
    static bool last[sizeof(actions)/sizeof(actions[0])]{};
    bool press[sizeof(actions)/sizeof(actions[0])]{};
    for (size_t i=0;i<sizeof(actions)/sizeof(actions[0]);++i) { bool now=glfwGetKey(w,actions[i])==GLFW_PRESS; press[i]=now&&!last[i]; last[i]=now; }
    if (ui_requested()) return;
    if (press[0]) camera_zoom(1.12f);
    if (press[1]) camera_zoom(1.f/1.12f);
    if (press[2]) camera_reset();
    if (press[3]) {show_demo=false;bg_index=(bg_index+1)%4;}
    if (press[4] && !ui_requested()) show_demo=!show_demo;
    if (press[5]) {stats=!stats;frames=0;work_ms=0;stats_start=Clock::now();cpu_start=std::clock();}
    if (press[6]) camera_pan(0,.08f);
    if (press[7]) camera_pan(-.08f,0);
    if (press[8]) camera_pan(0,-.08f);
    if (press[9]) camera_pan(.08f,0);
    if (press[10]) glfwSetWindowShouldClose(w,GLFW_TRUE);
}
const parameters::Runtime& editor_runtime(){return parameter_runtime;}
const std::vector<float>& editor_values(){return editor_current;}
bool editor_apply_mapping(const parameters::Mapping& map,std::string& error){
    if(!parameter_runtime.replace_mapping(map,error))return false;
    editor_changed=true;snapshot_tick=Clock::time_point{};return true;
}
bool editor_dirty(){return editor_changed;}
bool editor_can_save(){
    const auto& s=active_selection();
    return editor_map_valid&&s.editable&&!s.mapping.empty();
}
bool editor_save(std::string& error){
    if(!editor_can_save()){error="No writable profile map (external or invalid map).";return false;}
    const std::string path=active_selection().mapping;
    struct stat st{};
    if(::lstat(path.c_str(),&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=::getuid()||st.st_nlink!=1||(st.st_mode&0022)){
        error="Unsafe mapping file; not overwritten.";return false;
    }
    std::string pattern=path+".tmp-XXXXXX";
    std::vector<char> temp(pattern.begin(),pattern.end());temp.push_back(0);
    int fd=::mkstemp(temp.data());
    if(fd<0){error="Could not create temporary mapping file.";return false;}
    const std::string content=parameter_runtime.serialize_mappings();
    FILE* f=::fdopen(fd,"wb");bool ok=false;
    if(f){ok=std::fwrite(content.data(),1,content.size(),f)==content.size();
        if(std::fflush(f)!=0)ok=false;
        if(::fsync(fd)!=0)ok=false;
        if(std::fclose(f)!=0)ok=false;
    }else ::close(fd);
    if(ok)ok=std::rename(temp.data(),path.c_str())==0;
    if(!ok){::unlink(temp.data());error="Could not save map; previous file retained.";return false;}
    editor_changed=false;error.clear();return true;
}
void editor_preview(size_t target,float value){
    const auto& defs=parameter_runtime.definitions();
    if(target>=defs.size()||!std::isfinite(value))return;
    editor_target=int(target);editor_test_value=parameters::bound_value(defs[target],value);
}
void editor_release(){editor_target=-1;}
bool expression_preview(const std::string& packet){return parameter_runtime.ingest(packet,parameters::Clock::now());}
void expression_preview_clear(){parameter_runtime.clear_expression_preview();}
bool hot_switch_enabled(){return env_flag("STUDIO_HOT_SWITCH")&&!env_string("STUDIO_LIBRARY_DIR").empty();}
std::string expression_ui_directory(){const auto& p=parameter_socket.path();return p.empty()?std::string{}:p.substr(0,p.rfind('/'));}
const std::string& active_original_model(){return active_selection().original;}
const std::string& active_runtime_model(){return active_selection().runtime;}
std::string animation_profile_path(){const auto& s=active_selection();return s.editable&&!s.mapping.empty()?s.mapping+".animations":std::string{};}
std::string physics_profile_path(){const auto& s=active_selection();return s.editable&&!s.mapping.empty()?s.mapping+".physics":std::string{};}
bool take_model_selection(ModelSelection& selection){
    if(!hot_switch_enabled())return false;
    const std::string path=env_string("STUDIO_LIBRARY_DIR")+"/load.txt";
    std::ifstream f(path,std::ios::binary);if(!f)return false;
    std::string editable,extra;
    const bool has_base=bool(std::getline(f,selection.original)&&std::getline(f,selection.runtime)&&
                             std::getline(f,selection.mapping)&&std::getline(f,editable));
    selection.view.clear();
    const bool has_extra=std::getline(f,selection.view)&&std::getline(f,extra);
    f.close();if(std::remove(path.c_str())!=0)return false;
    const auto safe=[](const std::string& s){return !s.empty()&&s.size()<=8192&&s.front()=='/'&&s.find_first_of("\r\n\t")==std::string::npos&&s.find('\0')==std::string::npos;};
    if(!has_base||has_extra||!safe(selection.original)||!safe(selection.runtime)||
       (!selection.mapping.empty()&&!safe(selection.mapping))||(!selection.view.empty()&&!safe(selection.view))||
       (editable!="0"&&editable!="1")){
        std::puts("[model-switch] rejected: invalid prepared selection");std::fflush(stdout);return false;
    }
    selection.editable=editable=="1";return true;
}
void commit_model_selection(const ModelSelection& selection){
    camera_save();
    parameter_socket.close_socket();reset_controls();selection_committed=true;active_selection()=selection;
    load_model_view(selection.view);invalid_packets=0;
}
void reset_controls() { editor_release();editor_current.clear();editor_changed=false;editor_map_valid=false; selected = 0; initialized = false; overrides.clear(); previous.fill(false); position_motion={}; parameter_runtime.reset({}); parameter_socket.drain([](const std::string&){}); }
void update_controls(Model* m, GLFWwindow* w) {
    if (!m) return;
    std::array<bool, keys.size()> pressed{};
    for (size_t i=0; i<keys.size(); ++i) {
        bool now = w && !no_preview_requested() && glfwGetKey(w, keys[i]) == GLFW_PRESS;
        pressed[i] = now && !previous[i];
        previous[i] = now;
    }
    if (ui_requested()) pressed.fill(false);
    if (pressed[6]) glfwSetWindowShouldClose(w, GLFW_TRUE);
    int count = m->GetParameterCount();
    if (count <= 0) return;
    bool changed = !initialized;
    if (!initialized) {
        std::vector<parameters::Definition> defs;
        unsigned repeat_count=0;
        for(int i=0;i<count;++i) {
            const bool repeat=m->IsRepeat(i);
            defs.emplace_back(id(m,i),m->GetParameterMinimumValue(i),m->GetParameterMaximumValue(i),m->GetParameterDefaultValue(i),repeat);
            if(repeat) {
                ++repeat_count;
                std::fprintf(stdout,"[repeat] %s: %.4f .. %.4f\n",id(m,i),m->GetParameterMinimumValue(i),m->GetParameterMaximumValue(i));
            }
        }
        std::fprintf(stdout,"[repeat] %u cyclic parameters (model settings)\n",repeat_count);
        parameter_runtime.reset(std::move(defs));
        const auto& path=active_selection().mapping;
        if(!path.empty()) {
            std::ifstream map(path);std::string error;
            if(!map||!parameter_runtime.load_map(map,error)) std::fprintf(stderr,"[parameters] mapping not loaded: %s (identity mapping retained)\n",error.empty()?path.c_str():error.c_str());
            else {editor_map_valid=true;std::fprintf(stdout,"[parameters] mapping loaded: %s\n",path.c_str());}
        }
        if(env_flag("STUDIO_PARAMETER_INPUT")&&!parameter_socket.open_socket()) std::fprintf(stderr,"[parameters] could not open local input socket\n");
        parameter_tick=Clock::now(); snapshot_tick=Clock::time_point{}; list(m); initialized=true;
    } else if(pressed[5]) list(m);
    const auto now=Clock::now();
    parameter_socket.drain([&](const std::string& packet){
        if(!parameter_runtime.ingest(packet,now)&&++invalid_packets<=5) std::fprintf(stderr,"[parameters] rejected malformed or unmapped packet\n");
    });
    parameter_runtime.apply(now,std::chrono::duration<float>(now-parameter_tick).count(),
        [&](size_t i){return m->GetParameterValue(static_cast<int>(i));},
        [&](size_t i,float v){m->SetParameterValue(static_cast<int>(i),v);});
    parameter_tick=now;
    if (pressed[0]) { selected = (selected+count-1)%count; changed = true; }
    if (pressed[1]) { selected = (selected+1)%count; changed = true; }
    if (pressed[4]) { overrides.erase(selected); changed = true; }
    if (pressed[2] || pressed[3]) {
        float lo=m->GetParameterMinimumValue(selected), hi=m->GetParameterMaximumValue(selected);
        auto it = overrides.find(selected);
        float value = it == overrides.end() ? m->GetParameterValue(selected) : it->second;
        value += (pressed[3] ? 1.f : -1.f)*(hi-lo)/20.f;
        overrides[selected] = parameters::bound_value({id(m,selected),lo,hi,m->GetParameterDefaultValue(selected),m->IsRepeat(selected)},value);
        changed = true;
    }
    for (const auto& item: overrides) m->SetParameterValue(item.first, item.second);
    if(editor_target>=0&&editor_target<count)m->SetParameterValue(editor_target,editor_test_value);
    if (changed && w && !no_preview_requested() && !ui_requested()) title(m,w);
}
void apply_manual_overrides(Model* m) {
    if(!m)return;
    for(const auto& item:overrides)m->SetParameterValue(item.first,item.second);
    const int count=m->GetParameterCount();
    if(editor_target>=0&&editor_target<count)m->SetParameterValue(editor_target,editor_test_value);
    if(ui_requested()){
        editor_current.resize(count);
        for(int i=0;i<count;++i)editor_current[i]=m->GetParameterValue(i);
    }
    publish_parameters(m);
}
void begin_frame() {
    frame_start = Clock::now();
    if (frames==0) { stats_start=frame_start;cpu_start=std::clock(); }
}
void apply_live_broadcast_settings(int fps,bool show_stats){
    if(fps==0||fps==30||fps==60||fps==120||fps==144)frame_limit=fps;
    if(stats!=show_stats){stats=show_stats;frames=0;work_ms=0;stats_start=Clock::now();cpu_start=std::clock();}
}
void pace_frame() {
    // The supervisor publishes live settings even when the WSI/UI is hidden.
    static const std::string settings_dir=env_string("STUDIO_LIBRARY_DIR");
    static auto next_poll=Clock::time_point{};
    static UiBroadcast last_settings;
    static bool seen=false;
    const auto now_poll=Clock::now();
    if(!settings_dir.empty()&&now_poll>=next_poll){
        next_poll=now_poll+std::chrono::milliseconds(150);
        std::ifstream f(settings_dir+"/broadcast-current.tsv");UiBroadcast v;
        if(f&&ui_read_broadcast(f,v)&&(!seen||v.fps!=last_settings.fps||v.stats!=last_settings.stats)){
            apply_live_broadcast_settings(v.fps,v.stats);last_settings=v;seen=true;
        }
    }
    const int fps=frame_limit;
    const auto before_pace=Clock::now();
    // No presentation throttling in background; never spin at unlimited FPS there.
    const int effective_fps = (fps == 0 && background_frame()) ? 60 : fps;
    if (effective_fps) std::this_thread::sleep_until(frame_start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/effective_fps)));
    if (!stats) return;
    ++frames;
    work_ms+=std::chrono::duration<double,std::milli>(before_pace-frame_start).count();
    const auto now=Clock::now();
    const double elapsed=std::chrono::duration<double>(now-stats_start).count();
    if (elapsed>=1.0) {
        const auto cpu_now=std::clock();
        measured_fps=frames/elapsed;
        const double cpu_percent=100.0*(cpu_now-cpu_start)/CLOCKS_PER_SEC/elapsed;
        std::fprintf(stdout,"[perf] %.1f FPS | work/wait %.2f ms | CPU %.1f%% (one core=100%%) | RSS %.1f MiB | %s\n",measured_fps,work_ms/frames,cpu_percent,rss_mib(),background_frame()?"background":(offscreen()?"offscreen":"direct"));
        std::fflush(stdout);frames=0;work_ms=0;stats_start=now;cpu_start=cpu_now;
    }
}
}
