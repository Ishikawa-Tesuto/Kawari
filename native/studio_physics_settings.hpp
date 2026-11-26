#pragma once
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <utility>

namespace studio {
namespace physics {
struct Group {
    std::string id, name;
    float factor=1.f;
    Group(std::string i,std::string n):id(std::move(i)),name(std::move(n)){}
};
struct Settings {
    bool enabled=true;
    float strength=1.f;
    int fps=0; // 0: exported model setting, including variable timestep if absent.
    std::vector<Group> groups;
    void reset(){enabled=true;strength=1.f;fps=0;for(auto& g:groups)g.factor=1.f;}
};
inline bool valid_factor(float f){return std::isfinite(f)&&f>=0.f&&f<=2.f;}
inline bool valid_fps(int f){return f==0||f==30||f==60||f==120;}
inline bool safe_field(const std::string& s){return !s.empty()&&s.size()<=8192&&s.find_first_of("\t\r\n")==std::string::npos&&s.find('\0')==std::string::npos;}
inline bool same(const Settings& a,const Settings& b){
    if(a.enabled!=b.enabled||a.strength!=b.strength||a.fps!=b.fps||a.groups.size()!=b.groups.size())return false;
    for(size_t i=0;i<a.groups.size();++i)if(a.groups[i].id!=b.groups[i].id||a.groups[i].factor!=b.groups[i].factor)return false;
    return true;
}
inline bool serialize(const Settings& s,const std::string& model,std::string& result){
    if(!safe_field(model)||!valid_factor(s.strength)||!valid_fps(s.fps))return false;
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    out<<"kawari-physics-1\n"<<model<<'\n'<<int(s.enabled)<<' '<<s.strength<<' '<<s.fps<<'\n';
    for(const auto& g:s.groups){if(!safe_field(g.id)||!valid_factor(g.factor))return false;out<<g.id<<'\t'<<g.factor<<'\n';}
    result=out.str();return true;
}
inline bool parse(const std::string& contents,const std::string& model,Settings& s){
    if(contents.size()>1024*1024||!safe_field(model))return false;
    Settings next=s;next.reset();std::istringstream in(contents);in.imbue(std::locale::classic());std::string line,extra;
    if(!std::getline(in,line)||line!="kawari-physics-1"||!std::getline(in,line)||line!=model||!std::getline(in,line))return false;
    std::istringstream head(line);head.imbue(std::locale::classic());int enabled;
    if(!(head>>enabled>>next.strength>>next.fps)||(head>>extra)||(enabled!=0&&enabled!=1)||!valid_factor(next.strength)||!valid_fps(next.fps))return false;
    next.enabled=enabled!=0;std::vector<bool> seen(next.groups.size(),false);
    while(std::getline(in,line)){
        const auto tab=line.find('\t');if(tab==std::string::npos)return false;
        const auto id=line.substr(0,tab);const auto it=std::find_if(next.groups.begin(),next.groups.end(),[&](const Group& g){return g.id==id;});
        if(it==next.groups.end())return false;
        const size_t i=size_t(it-next.groups.begin());if(seen[i])return false;seen[i]=true;
        std::istringstream value(line.substr(tab+1));value.imbue(std::locale::classic());
        if(!(value>>it->factor)||(value>>extra)||!valid_factor(it->factor))return false;
    }
    if(std::find(seen.begin(),seen.end(),false)!=seen.end())return false;
    s=std::move(next);return true;
}
inline bool safe_file(const struct stat& st){return S_ISREG(st.st_mode)&&st.st_uid==::getuid()&&st.st_nlink==1&&!(st.st_mode&0077);}
// Missing is not an error. Invalid or unsafe files are never applied.
inline bool load(const std::string& path,const std::string& model,Settings& s,std::string& error){
    error.clear();if(path.empty())return true;
    int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT)return true;error="Could not open physics settings safely.";return false;}
    struct stat st{};if(::fstat(fd,&st)!=0||!safe_file(st)||st.st_size<0||st.st_size>1024*1024){::close(fd);error="Unsafe physics settings file.";return false;}
    std::string data;char buf[4096];ssize_t n;
    do {n=::read(fd,buf,sizeof(buf));if(n>0)data.append(buf,size_t(n));}while((n>0|| (n<0&&errno==EINTR))&&data.size()<=1024*1024);
    ::close(fd);if(n!=0||!parse(data,model,s)){error="Invalid or outdated physics settings; model defaults retained.";return false;}return true;
}
inline bool save(const std::string& path,const std::string& model,const Settings& s,std::string& error){
    std::string data;error.clear();if(path.empty()||!serialize(s,model,data)){error="No writable managed physics profile.";return false;}
    struct stat st{};const int found=::lstat(path.c_str(),&st);
    if((found==0&&!safe_file(st))||(found!=0&&errno!=ENOENT)){error="Unsafe physics settings file; not overwritten.";return false;}
    std::string pattern=path+".tmp-XXXXXX";std::vector<char> temp(pattern.begin(),pattern.end());temp.push_back(0);
    int fd=::mkstemp(temp.data());if(fd<0){error="Could not create temporary physics settings.";return false;}
    FILE* f=::fdopen(fd,"wb");bool ok=false;
    if(f){ok=std::fwrite(data.data(),1,data.size(),f)==data.size();if(std::fflush(f)!=0)ok=false;if(::fsync(fd)!=0)ok=false;if(std::fclose(f)!=0)ok=false;}else ::close(fd);
    if(ok)ok=::rename(temp.data(),path.c_str())==0;
    if(!ok){::unlink(temp.data());error="Could not save physics settings; previous file retained.";}
    return ok;
}
}
}
