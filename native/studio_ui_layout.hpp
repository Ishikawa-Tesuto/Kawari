#pragma once
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <locale>
#include <cmath>
#include <utility>
#include <set>
#include <map>
#include <cstdint>
namespace studio {
// An authoritative API sample, not a locally started reward timer.
struct UiExpressionTimer {uint64_t remaining=0,sampled=0;};
inline uint64_t ui_expression_remaining(const UiExpressionTimer& t,uint64_t now){
    if(now<t.sampled||now-t.sampled>1500||now-t.sampled>=t.remaining)return 0;
    return t.remaining-(now-t.sampled);
}
inline std::map<std::string,UiExpressionTimer> ui_expression_timers(std::istream& in,const std::string& model){
    std::map<std::string,UiExpressionTimer> result;std::string line,identity;
    while(std::getline(in,line)){
        std::istringstream row(line);row.imbue(std::locale::classic());std::string kind,key;
        if(!std::getline(row,kind,'\t')||!std::getline(row,key,'\t'))continue;
        if(kind=="M"){identity=key;continue;}
        if(kind!="Q")continue;
        uint64_t remaining=0,sampled=0;std::string extra;
        if(row>>remaining>>sampled&&!(row>>extra)&&remaining>0&&remaining<=86400000&&sampled>0){UiExpressionTimer t;t.remaining=remaining;t.sampled=sampled;result[key]=t;}
    }
    if(identity.empty()||identity!=model)result.clear();
    return result;
}
// Search labels only: keep the actual Cubism parameter IDs case-sensitive.
// Fold ASCII and Cyrillic UTF-8 without changing identifiers or process locale.
inline std::string ui_search_fold(std::string s) {
    for(size_t i=0;i<s.size();++i){
        const auto a=static_cast<unsigned char>(s[i]);
        if(a>='A'&&a<='Z'){s[i]=char(a+32);continue;}
        if(a!=0xd0||i+1>=s.size())continue;
        const auto b=static_cast<unsigned char>(s[i+1]);
        if(b>=0x80&&b<=0x8f){s[i]=char(0xd1);s[++i]=char(b+0x10);}
        else if(b>=0x90&&b<=0x9f){s[++i]=char(b+0x20);}
        else if(b>=0xa0&&b<=0xaf){s[i]=char(0xd1);s[++i]=char(b-0x20);}
    }
    return s;
}
inline bool ui_parameter_matches(const std::string& id, const char* query) {
    if (!query || !*query) return true;
    return ui_search_fold(id).find(ui_search_fold(query))!=std::string::npos;
}
// Display name only; keep the original full path as model identity.
inline std::string ui_model_label(const std::string& path){
    auto name=path.substr(path.find_last_of('/')+1);
    const std::string suffix=".model3.json";
    if(name.size()>suffix.size()&&name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0)name.resize(name.size()-suffix.size());
    return name;
}
// Catalog from the model-scoped private API mailbox. No synthetic tracking.
struct UiInput {
    std::string name,kind,owner;
    float minimum=0,maximum=0,initial=0,value=0;
    bool live=false,routed=false;
};
inline bool ui_input_number(const std::string& s,float& v){
    std::istringstream in(s);in.imbue(std::locale::classic());char extra;
    return (in>>v)&&!(in>>extra)&&std::isfinite(v);
}
inline bool ui_parse_inputs(std::istream& in,std::vector<UiInput>& out){
    std::string line;std::vector<UiInput> next;std::set<std::string> names;
    if(!std::getline(in,line)||line!="kawari-inputs-1")return false;
    size_t bytes=line.size();
    while(std::getline(in,line)){
        bytes+=line.size()+1;if(bytes>1024*1024||next.size()>=4096)return false;
        std::vector<std::string> f;std::istringstream row(line);std::string part;
        while(std::getline(row,part,'\t'))f.push_back(part);
        if(f.size()!=10||f[0]!="I"||f[1].empty()||f[1].size()>128||f[1].find_first_of(" \r\n#")!=std::string::npos||!names.insert(f[1]).second)return false;
        UiInput p;p.name=f[1];p.kind=f[2];p.owner=f[3];
        if(p.kind!="builtin"&&p.kind!="plugin"&&p.kind!="mapping")return false;
        if(!ui_input_number(f[4],p.minimum)||!ui_input_number(f[5],p.maximum)||!ui_input_number(f[6],p.initial)||!ui_input_number(f[7],p.value)||p.minimum>=p.maximum)return false;
        if((f[8]!="0"&&f[8]!="1")||(f[9]!="0"&&f[9]!="1"))return false;
        p.live=f[8]=="1";p.routed=f[9]=="1";next.push_back(std::move(p));
    }
    if(in.bad())return false;
    out.swap(next);return true;
}
// Range direction is preserved, including reversed calibration ranges.
inline float ui_range_position(float value,float start,float end){
    if(!std::isfinite(value)||!std::isfinite(start)||!std::isfinite(end)||start==end)return 0.f;
    return float(std::max(0.,std::min(1.,(double(value)-start)/(double(end)-start))));
}
// Stable GLFW key tokens, not ImGui enum ordinals or localized text.
// v2: bit 16 marks the format, bits 9..12 hold Shift/Ctrl/Alt/Super,
// bits 0..8 hold the GLFW key. Zero removes the shortcut.
inline bool shortcut_key(unsigned k) {
    return k==32||k==39||(k>=44&&k<=57)||k==59||k==61||(k>=65&&k<=93)||k==96||k==161||k==162||
        (k>=256&&k<=269)||(k>=280&&k<=284)||(k>=290&&k<=314)||(k>=320&&k<=336)||k==348;
}
inline unsigned shortcut_encode(unsigned key,unsigned mods) {
    return shortcut_key(key)&&mods<=15&&!(key==256&&mods==0)?(0x10000u|(mods<<9)|key):0;
}
inline bool shortcut_normalize(unsigned value,unsigned& out) {
    if(value==0){out=0;return true;}
    if(value<128){unsigned k=value&15u;if(k<1||k>8)return false;
        out=shortcut_encode(293+k,((value&16)?2:0)|((value&32)?1:0)|((value&64)?4:0));return true;}
    if((value&~0x11fffu)!=0||(value&0x10000u)==0)return false;
    out=shortcut_encode(value&511u,(value>>9)&15u);return out==value;
}
inline std::string shortcut_label(unsigned chord) {
    unsigned v=0;if(!shortcut_normalize(chord,v)||!v)return "Unassigned";
    unsigned k=v&511u,m=(v>>9)&15u;std::string s;
    if(m&2)s+="Ctrl + ";
    if(m&1)s+="Shift + ";
    if(m&4)s+="Alt + ";
    if(m&8)s+="Super + ";
    if(k>=290&&k<=314)return s+"F"+std::to_string(k-289);
    if(k>=320&&k<=329)return s+"NumPad "+std::to_string(k-320);
    if(k>=330&&k<=336){const char* labels[]={"Decimal","/","*","-","+","Enter","="};return s+"NumPad "+labels[k-330];}
    if(k>=256&&k<=269){const char* labels[]={"Esc","Enter","Tab","Backspace","Insert","Delete","Right","Left","Down","Up","Page Up","Page Down","Home","End"};return s+labels[k-256];}
    if(k>=280&&k<=284){const char* labels[]={"Caps Lock","Scroll Lock","Num Lock","Print Screen","Pause"};return s+labels[k-280];}
    if(k==32)return s+"Space";
    if(k==348)return s+"Menu";
    if(k==161||k==162)return s+"World "+std::to_string(k-160);
    return s+char(k);
}
// Wire format shared with the Rust supervisor. No model-specific state here.
struct UiBroadcast {
    int width=1920,height=1080,fps=60,bfps=60,mask=1024,ssaa=1;
    bool no_preview=false,stats=false;
    bool valid()const{
        return width>=320&&width<=7680&&height>=320&&height<=7680&&
            (fps==0||fps==30||fps==60||fps==120||fps==144)&&
            (bfps==30||bfps==60||bfps==120||bfps==144)&&
            (mask==256||mask==512||mask==1024||mask==2048)&&
            (ssaa==1||(ssaa==2&&width<=3840&&height<=3840));
    }
    bool operator==(const UiBroadcast& b)const{return width==b.width&&height==b.height&&fps==b.fps&&bfps==b.bfps&&mask==b.mask&&ssaa==b.ssaa&&no_preview==b.no_preview&&stats==b.stats;}
    bool restart_diff(const UiBroadcast& b)const{return width!=b.width||height!=b.height||bfps!=b.bfps||mask!=b.mask||ssaa!=b.ssaa||no_preview!=b.no_preview;}
};
inline bool ui_read_broadcast(std::istream& in,UiBroadcast& out){
    UiBroadcast v;std::string version,extra;int preview=0,stats=0;
    if(!(in>>version>>v.width>>v.height>>v.fps>>v.bfps>>v.mask>>v.ssaa>>preview>>stats)||version!="kawari-broadcast-1"||preview<0||preview>1||stats<0||stats>1||(in>>extra)||!v.valid())return false;
    v.no_preview=preview!=0;v.stats=stats!=0;out=v;return true;
}
inline std::string ui_write_broadcast(const UiBroadcast& v){
    std::ostringstream out;out.imbue(std::locale::classic());out<<"kawari-broadcast-1\n"<<v.width<<' '<<v.height<<' '<<v.fps<<' '<<v.bfps<<' '<<v.mask<<' '<<v.ssaa<<' '<<int(v.no_preview)<<' '<<int(v.stats)<<'\n';return out.str();
}
inline bool ui_flag(const char* key) {
    const char* v=std::getenv(key); return v && std::strcmp(v,"1")==0;
}
inline bool& ui_keyboard_capture() { static bool captured=false; return captured; }
inline bool& ui_available() { static bool enabled=true; return enabled; }
inline bool ui_requested() {
    return ui_available() && ui_flag("STUDIO_UI") && ui_flag("STUDIO_BROADCAST") && !ui_flag("STUDIO_NO_PREVIEW");
}
// Preview layout only; never changes the master canvas or camera transform.
inline bool& ui_fill_preview() { static bool fill=false; return fill; }
inline bool& ui_compact_bars() { static bool compact=false; return compact; }
// Preview workspace is expressed in logical pixels and projected into the
// actual framebuffer independently. No camera or master canvas changes.
struct UiWorkspace {
    float width=0,height=0,left=0,right=0,bar=0;
};
inline UiWorkspace& ui_workspace(){static UiWorkspace value;return value;}
inline UiWorkspace ui_workspace_layout(float w,float h,bool compact,float preferred_dock,float scale=1.f){
    UiWorkspace a;a.width=w;a.height=h;
    if(w<=0||h<=0)return a;
    a.bar=std::max(0.f,std::min(36.f*scale,(h-1.f)/3.f));
    if(!compact){
        a.left=std::min(52.f*scale,w*.32f);
        // Leave room to interact with the preview alongside the dock.
        if(preferred_dock>0&&w>=960.f&&h>=420.f)
            a.right=std::min(std::max(320.f,preferred_dock),std::min((w-a.left)*.60f,w-a.left-300.f));
    }
    return a;
}
struct UiPreviewArea {float x,y,w,h,input_right;};
inline UiPreviewArea ui_preview_area(int w,int h){
    if(!ui_requested())return {0,0,float(w),float(h),float(w)};
    const auto& a=ui_workspace();
    if(a.width>0&&a.height>0){
        const float sx=float(w)/a.width,sy=float(h)/a.height;
        // The dock covers the preview but only shortens the mouse input area.
        return {a.left*sx,a.bar*sy,(a.width-a.left)*sx,(a.height-2*a.bar)*sy,float(w)-a.right*sx};
    }
    const int bar=std::max(0,std::min(ui_compact_bars()?32:64,(h-1)/3));
    return {0,float(bar),float(w),float(h-2*bar),float(w)};
}
inline int ui_bar_height(int h) { return int(ui_preview_area(1,h).y); }
inline int ui_preview_height(int h) { return int(ui_preview_area(1,h).h); }
}
