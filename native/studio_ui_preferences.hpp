#pragma once
#include <array>
#include <string>
#include <sstream>
#include <locale>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <cmath>
#include <algorithm>
namespace studio { namespace ui {
// Global UI-only state. Never contains model settings, credentials or camera state.
struct ToolLayout { int width=520; bool floating=false,open=false; int x=0,y=0,w=0,h=0; };
struct Preferences {
    int scale=100; bool russian=false,stream=false,fill=false;
    std::array<ToolLayout,7> tools;
    Preferences(){const int widths[]={720,520,560,520,530,510,440};for(size_t i=0;i<tools.size();++i)tools[i].width=widths[i];}
};
inline Preferences& preferences(){static Preferences p;return p;}
inline bool valid(const Preferences& p){
    if(p.scale!=85&&p.scale!=100&&p.scale!=115&&p.scale!=130&&p.scale!=150)return false;
    int dock=0;for(const auto& t:p.tools){if(t.open&&!t.floating)++dock;
        if(t.width<320||t.width>3000||t.x < -32768||t.x>32768||t.y < -32768||t.y>32768||t.w<0||t.w>32768||t.h<0||t.h>32768||((t.w==0)!=(t.h==0)))return false;}
    return dock<=1;
}
inline std::string encode_preferences(const Preferences& p){
    std::ostringstream o;o.imbue(std::locale::classic());o<<"kawari-ui-2\n"<<p.scale<<' '<<p.russian<<' '<<p.stream<<' '<<p.fill<<'\n';
    for(const auto& t:p.tools)o<<t.width<<' '<<t.floating<<' '<<t.open<<' '<<t.x<<' '<<t.y<<' '<<t.w<<' '<<t.h<<'\n';
    return o.str();
}
inline bool decode_preferences(const std::string& s,Preferences& p){
    if(s.size()>4096)return false;
    Preferences v;std::istringstream i(s);i.imbue(std::locale::classic());std::string version,extra;
    if(!(i>>version>>v.scale>>v.russian>>v.stream>>v.fill)||(version!="kawari-ui-1"&&version!="kawari-ui-2"))return false;
    for(size_t n=0;n<(version=="kawari-ui-1"?6:v.tools.size());++n){auto& t=v.tools[n];if(!(i>>t.width>>t.floating>>t.open>>t.x>>t.y>>t.w>>t.h))return false;}
    if((i>>extra)||!valid(v))return false;
    p=v;return true;
}
inline std::string preferences_path(){const char* x=std::getenv("XDG_DATA_HOME"),*h=std::getenv("HOME");if(x&&*x)return std::string(x)+"/kawari/ui.conf";return h&&*h?std::string(h)+"/.local/share/kawari/ui.conf":"";}
inline bool safe_pref(const struct stat& st){return S_ISREG(st.st_mode)&&st.st_uid==getuid()&&st.st_nlink==1&&!(st.st_mode&0077);}
inline bool read_preferences(const std::string& path,Preferences& p){
    if(path.empty())return false;
    int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)return errno==ENOENT;
    struct stat st{};if(fstat(fd,&st)||!safe_pref(st)||st.st_size<0||st.st_size>4096){close(fd);return false;}
    std::string s;char buf[512];ssize_t n;do{n=read(fd,buf,sizeof(buf));if(n>0)s.append(buf,size_t(n));}while((n>0||(n<0&&errno==EINTR))&&s.size()<=4096);close(fd);
    return n==0&&decode_preferences(s,p);
}
inline bool write_preferences(const std::string& path,const Preferences& p){
    if(path.empty()||!valid(p))return false;
    struct stat st{};int r=lstat(path.c_str(),&st);if((r==0&&!safe_pref(st))||(r!=0&&errno!=ENOENT))return false;
    auto data=encode_preferences(p);std::string pat=path+".tmp-XXXXXX";std::vector<char> tmp(pat.begin(),pat.end());tmp.push_back(0);
    int fd=mkstemp(tmp.data());if(fd<0)return false;FILE* f=fdopen(fd,"wb");bool ok=false;
    if(f){ok=fwrite(data.data(),1,data.size(),f)==data.size();if(fflush(f)!=0)ok=false;if(fsync(fd)!=0)ok=false;if(fclose(f)!=0)ok=false;}else close(fd);
    if(ok)ok=rename(tmp.data(),path.c_str())==0;
    if(!ok)unlink(tmp.data());
    return ok;
}
inline const char* tr(const char* en,const char* ru){return preferences().russian?ru:en;}
inline const char* const* tool_titles(){static const char* titles[]={"Parameters###Parameter Editor","Physics","Animations","Expressions & Hotkeys","Integrations###Plugins","Settings###Broadcast Settings","Items"};return titles;}
inline int tool_index(const char* title){for(int i=0;i<7;++i)if(std::string(title)==tool_titles()[i])return i;return -1;}
inline const char* tool_ru(int i){static const char* names[]={"Параметры","Физика","Анимации","Выражения и клавиши","Интеграции","Настройки","Предметы"};return i>=0&&i<7?names[i]:"";}
inline std::string window_label(const char* title){const int i=tool_index(title);if(i<0)return title;std::string id=title;auto pos=id.find("###");if(pos!=std::string::npos)id=id.substr(pos+3);const auto visible=std::string(title).substr(0,std::string(title).find("###"));return (preferences().russian?std::string(tool_ru(i)):visible)+"###"+id;}
inline float scale(){return preferences().scale/100.f;}
inline void clamp_float(ToolLayout& t,float display_w,float display_h,float bar){
    if(!t.w||!t.h)return;
    t.w=int(std::max(1.f,std::min(float(t.w),display_w-20.f)));t.h=int(std::max(1.f,std::min(float(t.h),display_h-2*bar-12.f)));
    t.x=int(std::max(0.f,std::min(float(t.x),display_w-t.w)));t.y=int(std::max(0.f,std::min(float(t.y),display_h-t.h)));
}
}}
