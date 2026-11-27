#pragma once
#include "studio_parameters.hpp"
#include "studio_item_attachment.hpp"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <locale>
#include <iomanip>
#include <fstream>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace studio { namespace items {
struct MeshBinding {
    std::string model,id;
    Anchor anchor;
    explicit operator bool() const {return !id.empty();}
};
struct Item { std::string path; float x=0,y=0,size=.35f,angle=0,opacity=1; bool visible=true,attached=false,behind=false,locked=false; MeshBinding mesh; };
inline std::string asset_id(const Item& i){auto last=i.path.find_last_of('/');if(last==std::string::npos||last==0)return {};auto prev=i.path.find_last_of('/',last-1);auto id=i.path.substr(prev==std::string::npos?0:prev+1,last-(prev==std::string::npos?0:prev+1));return parameters::item_id_ok(id)?id:std::string{};}
inline Item effective(const Item& i,const parameters::Runtime& runtime,parameters::Clock::time_point now){Item out=i;auto id=asset_id(i);if(!id.empty()){float weight=runtime.item_visibility(id,i.visible,now);out.visible=weight>0.f;out.opacity=i.opacity*weight;}return out;}
struct Pose {float x,y,size,angle;};
inline Pose pose(const Item& i,float zoom,float panx,float pany){return i.attached?Pose{i.x*zoom+panx,i.y*zoom+pany,i.size*zoom,i.angle}:Pose{i.x,i.y,i.size,i.angle};}
inline void attach(Item& i,bool on,float z,float px,float py){if(i.attached==on||z<=0)return;auto p=pose(i,z,px,py);i.attached=on;i.x=on?(p.x-px)/z:p.x;i.y=on?(p.y-py)/z:p.y;i.size=on?p.size/z:p.size;}
inline Pose mesh_pose(const Item& item,const AttachmentFrame& frame,float aspect){
    const Point point=frame.origin+frame.offset({item.x,item.y});
    return {point.x/aspect,point.y,item.size*frame.scale,item.angle-frame.rotation()};
}
inline void fit_mesh(Item& item,const Pose& visible,const AttachmentFrame& frame,float aspect){
    const Point local=frame.inverse(Point(visible.x*aspect,visible.y)-frame.origin);
    item.x=local.x;item.y=local.y;item.size=visible.size/frame.scale;
    item.angle=std::remainder(visible.angle+frame.rotation(),360.f);
}
inline bool clean_field(const std::string& field,size_t limit){
    return !field.empty()&&field.size()<limit&&field.find_first_of("\r\n\t")==std::string::npos&&field.find('\0')==std::string::npos;
}
inline bool valid(const Item& i){
    return clean_field(i.path,4096)&&i.path[0]=='/'&&std::isfinite(i.x)&&std::isfinite(i.y)&&
           std::abs(i.x)<=10000&&std::abs(i.y)<=10000&&i.size>=.001f&&i.size<=10000&&
           std::isfinite(i.angle)&&std::abs(i.angle)<=360&&i.opacity>=0&&i.opacity<=1&&
           (!i.mesh||(!i.attached&&clean_field(i.mesh.model,4096)&&i.mesh.model[0]=='/'&&
                      clean_field(i.mesh.id,1024)&&i.mesh.anchor.valid()));
}
inline std::string encode(const std::vector<Item>& rows){
    std::ostringstream out;out.imbue(std::locale::classic());
    const bool has_mesh=std::any_of(rows.begin(),rows.end(),[](const Item& item){return bool(item.mesh);});
    out<<(has_mesh?"kawari-items-2\n":"kawari-items-1\n")<<std::setprecision(9);
    for(const auto& i:rows){
        out<<i.x<<' '<<i.y<<' '<<i.size<<' '<<i.angle<<' '<<i.opacity<<' '<<i.visible<<' '<<i.attached
           <<' '<<i.behind<<' '<<i.locked<<'\t'<<i.path;
        if(i.mesh){
            const auto& a=i.mesh.anchor;
            out<<'\t'<<i.mesh.model<<'\t'<<i.mesh.id<<'\t'<<a.triangle<<' '<<a.edge<<' '<<a.u<<' '<<a.v<<' '<<a.length;
        }
        out<<'\n';
    }
    return out.str();
}
inline bool decode(const std::string& text,std::vector<Item>& rows){
    if(text.size()>393216)return false;
    std::istringstream input(text);std::string line;
    if(!std::getline(input,line)||(line!="kawari-items-1"&&line!="kawari-items-2"))return false;
    const bool supports_mesh=line=="kawari-items-2";
    std::vector<Item> result;
    while(std::getline(input,line)){
        if(result.size()>=32)return false;
        std::istringstream fields(line);std::string numbers,extra;
        Item i;
        if(!std::getline(fields,numbers,'\t')||!std::getline(fields,i.path,'\t'))return false;
        std::istringstream values(numbers);values.imbue(std::locale::classic());
        if(!(values>>i.x>>i.y>>i.size>>i.angle>>i.opacity>>i.visible>>i.attached>>i.behind>>i.locked)||(values>>extra))return false;
        if(!fields.eof()){
            std::string anchor;
            if(!supports_mesh||!std::getline(fields,i.mesh.model,'\t')||!std::getline(fields,i.mesh.id,'\t')||
               !std::getline(fields,anchor)||!i.mesh)return false;
            std::istringstream geometry(anchor);geometry.imbue(std::locale::classic());
            auto& a=i.mesh.anchor;
            if(!(geometry>>a.triangle>>a.edge>>a.u>>a.v>>a.length)||(geometry>>extra))return false;
        }
        if(!valid(i))return false;
        result.push_back(std::move(i));
    }
    rows=std::move(result);
    return true;
}
inline std::string directory(){const char* x=getenv("XDG_DATA_HOME"),*h=getenv("HOME");return x&&*x?std::string(x)+"/kawari":h&&*h?std::string(h)+"/.local/share/kawari":"";}
inline bool safe(const struct stat& s){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&s.st_nlink==1&&!(s.st_mode&0077);}
inline bool read(const std::string& p,std::vector<Item>& v){int fd=open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)return errno==ENOENT;struct stat s{};if(fstat(fd,&s)||!safe(s)||s.st_size<0||s.st_size>393216){close(fd);return false;}std::string data;char b[4096];ssize_t n;do{n=::read(fd,b,sizeof(b));if(n>0)data.append(b,n);}while((n>0||(n<0&&errno==EINTR))&&data.size()<=393216);close(fd);return n==0&&decode(data,v);}
inline bool save(const std::string& p,const std::vector<Item>& v){if(p.empty()||v.size()>32)return false;for(auto& i:v)if(!valid(i))return false;struct stat s{};int r=lstat(p.c_str(),&s);if((r==0&&!safe(s))||(r&&errno!=ENOENT))return false;auto data=encode(v);std::string tmp=p+".tmp-XXXXXX";std::vector<char> b(tmp.begin(),tmp.end());b.push_back(0);int fd=mkstemp(b.data());if(fd<0)return false;FILE* f=fdopen(fd,"wb");bool ok=false;if(f){ok=fwrite(data.data(),1,data.size(),f)==data.size();if(fflush(f)||fsync(fd))ok=false;if(fclose(f))ok=false;}else close(fd);if(ok)ok=rename(b.data(),p.c_str())==0;if(!ok)unlink(b.data());return ok;}
struct Scene {std::vector<Item> rows;std::string error;bool loaded=false;void load(){if(loaded)return;loaded=true;auto d=directory();if(d.empty()||!read(d+"/items.conf",rows))error="Could not read items.conf";} bool persist(){auto d=directory();if(d.empty()||!save(d+"/items.conf",rows)){error="Could not save items.conf";return false;}error.clear();return true;}};
inline Scene& scene(){static Scene s;s.load();return s;}
struct Pixels {int w=0,h=0;std::vector<std::vector<unsigned char>> frames;std::vector<int> delay;};
bool load_pixels(const std::string& path,Pixels& p,std::string& error);
// Import into the application-owned library, not the model folder.
inline bool import_file(const std::string& path,Item& item,std::string& error){
    Item candidate;candidate.path=path;if(!valid(candidate)){error="Enter an absolute image path";return false;}
    Pixels check;if(!load_pixels(path,check,error))return false;
    auto root=directory();if(root.empty()){error="No data directory";return false;}
    auto dir=root+"/items";if(mkdir(dir.c_str(),0700)&&errno!=EEXIST){error="Could not create Items directory";return false;}
    struct stat st{};if(lstat(dir.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)){error="Unsafe Items directory";return false;}
    auto pat=dir+"/asset-XXXXXX";std::vector<char> tmp(pat.begin(),pat.end());tmp.push_back(0);if(!mkdtemp(tmp.data())){error="Could not allocate asset directory";return false;}
    auto name=path.substr(path.find_last_of('/')+1);candidate.path=std::string(tmp.data())+"/"+name;
    int in=open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);int out=open(candidate.path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);bool ok=in>=0&&out>=0;
    if(ok){struct stat ss{};ok=!fstat(in,&ss)&&S_ISREG(ss.st_mode)&&ss.st_size>0&&ss.st_size<=32*1024*1024;}
    char buf[65536];size_t total=0;while(ok){ssize_t n=::read(in,buf,sizeof(buf));if(n==0)break;if(n<0){if(errno==EINTR)continue;ok=false;break;}total+=size_t(n);if(total>32u*1024u*1024u){ok=false;break;}for(ssize_t off=0;off<n;){ssize_t m=write(out,buf+off,n-off);if(m<0&&errno==EINTR)continue;if(m<=0){ok=false;break;}off+=m;}}
    if(in>=0){close(in);}
    if(out>=0){if(fsync(out))ok=false;if(close(out))ok=false;}
    if(ok){check=Pixels{};ok=load_pixels(candidate.path,check,error);}
    if(!ok){unlink(candidate.path.c_str());rmdir(tmp.data());if(error.empty())error="Could not copy image";return false;}item=candidate;return true;
}
inline size_t frame_at(const Pixels& p,long long ms){if(p.frames.size()<2)return 0;long long total=0;for(int d:p.delay)total+=d;if(total<=0)return 0;ms=std::max(0LL,ms)%total;for(size_t i=0;i<p.delay.size();++i){if(ms<p.delay[i])return i;ms-=p.delay[i];}return 0;}
}}
