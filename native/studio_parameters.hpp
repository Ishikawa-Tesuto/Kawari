#pragma once
#include <cstdio>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace studio {
namespace parameters {
using Clock = std::chrono::steady_clock;
struct Definition {
    std::string id; float minimum, maximum, initial; bool repeat;
    Definition(std::string n,float lo,float hi,float v,bool r=false)
        :id(std::move(n)),minimum(lo),maximum(hi),initial(v),repeat(r) {}
};
// Match the SDK's inclusive endpoints; a value beyond either endpoint wraps.
// Use double for extrapolation and modulo to keep extreme finite inputs safe.
inline float bound_value(const Definition& d,double v) {
    const double lo=d.minimum,hi=d.maximum,span=hi-lo;
    if (!std::isfinite(v)) return d.initial;
    if (d.repeat && span>0 && std::isfinite(span)) {
        if(v>hi) v=lo+std::fmod(v-hi,span);
        else if(v<lo) v=hi-std::fmod(lo-v,span);
    }
    return static_cast<float>(std::max(lo,std::min(hi,v)));
}
// Tracking supplies absolute positions, not a requested number of turns.
// Smooth across the seam via the shortest arc instead of reversing a full turn.
inline float smooth_value(const Definition& d,float from,float to,float alpha) {
    if(alpha>=1) return to;
    if(!d.repeat) return from+alpha*(to-from);
    const double span=double(d.maximum)-d.minimum;
    double delta=double(to)-from;
    if(span>0 && std::isfinite(span)) {
        delta=std::fmod(delta,span);
        if(delta>span/2) delta-=span;
        else if(delta< -span/2) delta+=span;
    }
    return bound_value(d,double(from)+alpha*delta);
}
struct Mapping {
    std::string input; size_t target; float in_min, in_max, out_min, out_max, smoothing_ms;
    bool active=false; float filtered=0;
    bool clamp_input=true, clamp_output=false;
    Mapping(std::string n,size_t t,float a,float b,float c,float d,float s):input(std::move(n)),target(t),in_min(a),in_max(b),out_min(c),out_max(d),smoothing_ms(s) {}
};
struct Sample { float value; Clock::time_point expires; };
inline float mapped_value(const Definition& d,const Mapping& m,float input) {
    double t=(double(input)-m.in_min)/(double(m.in_max)-m.in_min);
    if(m.clamp_input||m.clamp_output) t=std::max(0.0,std::min(1.0,t));
    return bound_value(d,m.out_min+(double(m.out_max)-m.out_min)*t);
}
inline float clamp(float v,float a,float b) { return std::max(a,std::min(b,v)); }
inline bool name_ok(const std::string& n) {
    if(n.empty()||n.size()>128) return false;
    for(unsigned char c:n) if(c<=32||c==127||c=='#') return false;
    return true;
}
// Stable imported asset directory ID; never a file path or a scene index.
inline bool item_id_ok(const std::string& id) {
    if(id.size()!=12||id.compare(0,6,"asset-")!=0)return false;
    for(size_t i=6;i<id.size();++i){char c=id[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')))return false;}
    return true;
}
constexpr float min_scale=.2f, max_scale=5.f;
constexpr float pan_limit=10000.f;
struct ModelView {
    float x, y, scale;
    ModelView(float x=0, float y=0, float scale=1) : x(x), y(y), scale(scale) {}
    bool valid() const {
        return std::isfinite(x)&&std::isfinite(y)&&std::abs(x)<=pan_limit&&std::abs(y)<=pan_limit&&
               scale>=min_scale&&scale<=max_scale;
    }
    bool operator==(const ModelView& other) const {
        return x==other.x&&y==other.y&&scale==other.scale;
    }
};
struct ModelPosition {
    float x=0, y=0, speed=0, scale=0;
    explicit operator bool() const {return speed!=0.f;}
    bool operator==(const ModelPosition& other) const {
        return x==other.x&&y==other.y&&speed==other.speed&&scale==other.scale;
    }
    bool valid() const {
        return ModelView(x,y,scale==0.f?1.f:scale).valid()&&speed>=.05f&&speed<=20.f;
    }
    ModelView target(float base_scale) const { return ModelView(x,y,scale>0.f?scale:base_scale); }
};
inline float view_distance(const ModelView& from,const ModelView& to) {
    return std::hypot(std::hypot(to.x-from.x,to.y-from.y),std::log(to.scale/from.scale));
}
enum class PositionSource { Expression, Obs, Preview };
struct PositionRequest {
    ModelPosition position;
    PositionSource source=PositionSource::Expression;
};

struct PositionMotion {
    ModelView goal;
    float speed=0;
    PositionSource source=PositionSource::Expression;
    bool active=false;

    void start(const ModelView& target,float transition_speed,PositionSource origin){
        goal=target;speed=transition_speed;source=origin;active=true;
    }
    void advance(float dt,ModelView& view){
        if(!active||!std::isfinite(dt)||dt<=0.f)return;
        const float distance=view_distance(view,goal),step=speed*std::min(dt,.1f);
        if(distance<=step){view=goal;active=false;return;}
        const float fraction=step/distance;
        view.x+=(goal.x-view.x)*fraction;
        view.y+=(goal.y-view.y)*fraction;
        view.scale*=std::exp(std::log(goal.scale/view.scale)*fraction);
    }
};
class Runtime {
    std::vector<Definition> definitions_;
    std::vector<Mapping> maps_;
    std::array<std::unordered_map<std::string,Sample>,3> sources_;
    struct Term { size_t target; char blend; float value; };
    struct ItemTerm {std::string id; float visibility;};
    struct Expression {
        std::string key; std::vector<Term> terms; std::vector<ItemTerm> items;
        ModelPosition position;
        float weight=0, start=0, goal=1;
        int fade_ms=0, fade_out_ms=0; Clock::time_point changed, deadline=Clock::time_point::max();
    };
    std::vector<Expression> expressions_;
    PositionRequest pending_position_;
    static float weight_at(const Expression& e, Clock::time_point now) {
        const float t=e.fade_ms?clamp(std::chrono::duration<float,std::milli>(now-e.changed).count()/e.fade_ms,0,1):1;
        return e.start+(e.goal-e.start)*t;
    }
    static void stop(Expression& e,Clock::time_point now,int override_ms=-1) {
        if(e.goal==0) return;
        e.start=weight_at(e,now);e.fade_ms=override_ms>=0?override_ms:e.fade_out_ms;e.goal=0;e.changed=now;e.deadline=Clock::time_point::max();
    }
    bool expression(std::istringstream& row,Clock::time_point now) {
        std::string op,key,extra;
        if(!(row>>op)) return false;
        if(op=="reset") {if(row>>extra)return false;expressions_.clear();pending_position_={};return true;}
        if(!(row>>key)||!name_ok(key)) return false;
        auto it=std::find_if(expressions_.begin(),expressions_.end(),[&](const Expression& e){return e.key==key;});
        if(op=="off"||op=="off2") {int override_ms=-1;if(op=="off2"&&(!(row>>override_ms)||override_ms<0||override_ms>10000))return false;if(row>>extra)return false;if(it!=expressions_.end())stop(*it,now,override_ms);return true;}
        int fade,hold,fade_out;
        if((op!="on"&&op!="on2")||!(row>>fade>>hold)||fade<0||fade>10000||hold<0||hold>86400000) return false;
        fade_out=fade;if(op=="on2"&&(!(row>>fade_out)||fade_out<0||fade_out>10000))return false;
        Expression e;e.key=key;e.fade_ms=fade;e.fade_out_ms=fade_out;e.changed=now;
        if(hold) e.deadline=now+std::chrono::milliseconds(hold);
        std::string name;char blend;float value;
        while(row>>name) {
            if(!(row>>blend>>value)||!std::isfinite(value)||std::abs(value)>1e6f) return false;
            if(blend=='P'||blend=='V') {
                ModelPosition p;p.x=value;
                if(name!="position"||e.position||!(row>>p.y>>p.speed))return false;
                if(blend=='V'&&(!(row>>p.scale)||p.scale==0.f))return false;
                if(!p.valid())return false;
                e.position=p;continue;
            }
            if(blend=='I') {
                if(!item_id_ok(name)||(value!=0.f&&value!=1.f)||e.items.size()>=32||std::any_of(e.items.begin(),e.items.end(),[&](const ItemTerm& t){return t.id==name;}))return false;
                e.items.push_back({name,value});continue;
            }
            if((blend!='A'&&blend!='M'&&blend!='O')||e.terms.size()>=256)return false;
            size_t i=0;while(i<definitions_.size()&&definitions_[i].id!=name)++i;
            if(i==definitions_.size()||std::any_of(e.terms.begin(),e.terms.end(),[&](const Term& t){return t.target==i;}))return false;
            e.terms.push_back({i,blend,value});
        }
        const bool effects=!e.terms.empty()||!e.items.empty();
        const bool preview=key=="editor-preview", obs=key=="obs-position";
        if((!effects&&!e.position)||(obs&&effects))return false;
        if(effects&&!preview&&it==expressions_.end()&&
           std::count_if(expressions_.begin(),expressions_.end(),[](const Expression& a){return a.key!="editor-preview";})>=64)return false;

        // A live preview refresh only moves the model when its placement changes.
        if(e.position&&(!preview||op=="on2"||it==expressions_.end()||!(e.position==it->position))){
            pending_position_.position=e.position;
            pending_position_.source=preview?PositionSource::Preview:obs?PositionSource::Obs:PositionSource::Expression;
        }
        if(it!=expressions_.end()){e.start=weight_at(*it,now);expressions_.erase(it);}
        if(effects||preview)expressions_.push_back(std::move(e));
        return true;
    }
    static int source(const std::string& s) { return s=="tracking"?0:s=="plugin"?1:s=="test"?2:-1; }
public:
    PositionRequest take_position_request(){
        const auto request=pending_position_;
        pending_position_={};
        return request;
    }
    void cancel_position_request(PositionSource source){
        if(pending_position_.source==source)pending_position_={};
    }
    bool position_preview_active(Clock::time_point now) const {
        return std::any_of(expressions_.begin(),expressions_.end(),[&](const Expression& e){
            return e.key=="editor-preview"&&e.position&&e.goal>0&&e.deadline>now;
        });
    }
    void clear_expression_preview(){
        cancel_position_request(PositionSource::Preview);
        expressions_.erase(std::remove_if(expressions_.begin(),expressions_.end(),[](const Expression& e){
            return e.key=="editor-preview";
        }),expressions_.end());
    }
    // Use the same envelope and activation order for the image compositor.
    // Evaluate on a copy so a render query cannot mutate runtime or saved scene.
    float item_visibility(const std::string& id,bool base,Clock::time_point now) const {
        float v=base?1.f:0.f;
        for(const auto& source:expressions_){
            auto term=std::find_if(source.items.begin(),source.items.end(),[&](const ItemTerm& t){return t.id==id;});
            if(term==source.items.end())continue;
            float w;
            if(source.deadline<=now){Expression e=source;stop(e,e.deadline);w=weight_at(e,now);}else w=weight_at(source,now);
            v+=(term->visibility-v)*w;
        }
        return clamp(v,0.f,1.f);
    }
    const std::vector<Mapping>& mappings() const { return maps_; }
    const std::vector<Definition>& definitions() const { return definitions_; }
    // Return only a currently routed, non-expired sample; the API's other inputs
    // may exist without reaching the renderer.
    bool live_input(const std::string& name,Clock::time_point now,float& value,int& source_index) const {
        for(int i=2;i>=0;--i) {
            const auto it=sources_[i].find(name);
            if(it!=sources_[i].end()&&it->second.expires>now) {
                value=it->second.value;source_index=i;return true;
            }
        }
        return false;
    }
    bool replace_mapping(Mapping m,std::string& error) {
        if(m.target>=definitions_.size()||!name_ok(m.input)||
           !std::isfinite(m.in_min)||!std::isfinite(m.in_max)||m.in_min==m.in_max||
           !std::isfinite(m.out_min)||!std::isfinite(m.out_max)||
           !std::isfinite(m.in_max-m.in_min)||!std::isfinite(m.out_max-m.out_min)||
           !std::isfinite(m.smoothing_ms)||m.smoothing_ms<0||m.smoothing_ms>5000) {
            error="Invalid input ID, range or smoothing (0..5000 ms).";return false;
        }
        if(!name_ok(definitions_[m.target].id)) {error="Invalid target ID.";return false;}
        auto it=std::find_if(maps_.begin(),maps_.end(),[&](const Mapping& old){return old.target==m.target;});
        m.active=false;m.filtered=0;
        if(it==maps_.end())maps_.push_back(std::move(m));else *it=std::move(m);
        error.clear();return true;
    }
    std::string serialize_mappings() const {
        std::ostringstream out;out.imbue(std::locale::classic());
        out.precision(std::numeric_limits<float>::max_digits10);
        out<<"# Studio Parameter Editor: input target in_min in_max out_min out_max smoothing_ms vts clamp_input clamp_output\n";
        for(const auto& m:maps_) out<<m.input<<' '<<definitions_[m.target].id<<' '<<m.in_min<<' '<<m.in_max<<' '<<m.out_min<<' '<<m.out_max<<' '<<m.smoothing_ms<<" vts "<<int(m.clamp_input)<<' '<<int(m.clamp_output)<<'\n';
        return out.str();
    }
    void reset(std::vector<Definition> defs) {
        definitions_=std::move(defs); maps_.clear(); expressions_.clear(); pending_position_={};
        for(auto& s:sources_) s.clear();
        for(size_t i=0;i<definitions_.size();++i) {
            const auto& d=definitions_[i];
            if(d.minimum<d.maximum) {
                maps_.emplace_back(d.id,i,d.minimum,d.maximum,d.minimum,d.maximum,0);
                // Identity input is already in model units; do not clip a repeat.
                maps_.back().clamp_input=!d.repeat;
            }
        }
    }
    // One mapping per target. An explicit mapping replaces the default identity mapping.
    bool load_map(std::istream& file,std::string& error) {
        auto next=maps_; std::vector<bool> seen(definitions_.size(),false);
        std::string line; size_t line_no=0;
        while(std::getline(file,line)) {
            ++line_no; if(line.size()>2048) {error="line too long";return false;}
            std::istringstream row(line.substr(0,line.find('#'))); row.imbue(std::locale::classic());
            std::string in,target,extra; float a,b,c,d,s;
            if(!(row>>in)) continue;
            auto fail=[&](){error="invalid mapping at line "+std::to_string(line_no);return false;};
            if(!(row>>target>>a>>b>>c>>d>>s)||!name_ok(in)||!std::isfinite(a)||!std::isfinite(b)||!std::isfinite(c)||!std::isfinite(d)||!std::isfinite(s)||a==b||!std::isfinite(b-a)||!std::isfinite(d-c)||s<0||s>5000) return fail();
            bool vts=false; int ci=1,co=0;
            if(row>>extra) {
                if(extra!="vts"||!(row>>ci>>co)||(row>>extra)||(ci!=0&&ci!=1)||(co!=0&&co!=1)) return fail();
                vts=true;
            }
            if(!vts&&a>b) return fail();
            size_t i=0; while(i<definitions_.size()&&definitions_[i].id!=target) ++i;
            if(i==definitions_.size() || seen[i]) {
                if (!vts) return fail();
                std::fprintf(stderr,"[parameters] skipped VTS mapping line %zu: %s (%s)\n",line_no,target.c_str(),i==definitions_.size()?"unknown target":"duplicate target");
                continue;
            }
            if(!vts&&(c<definitions_[i].minimum||c>definitions_[i].maximum||d<definitions_[i].minimum||d>definitions_[i].maximum)) return fail();
            seen[i]=true;
            auto it=std::find_if(next.begin(),next.end(),[&](const Mapping& m){return m.target==i;});
            Mapping mapping(in,i,a,b,c,d,s); mapping.clamp_input=ci!=0; mapping.clamp_output=co!=0; if(it==next.end()) next.push_back(mapping); else *it=mapping;
        }
        if(file.bad()) {error="map read error";return false;}
        maps_=std::move(next); return true;
    }
    // Atomic datagram: set SOURCE TTL_MS INPUT VALUE [INPUT VALUE ...], or clear SOURCE.
    bool ingest(const std::string& message,Clock::time_point now) {
        if(message.empty()||message.size()>16384||message.find('\0')!=std::string::npos) return false;
        std::istringstream row(message); row.imbue(std::locale::classic());
        std::string op,src,extra; if(!(row>>op)) return false; if(op=="expr")return expression(row,now); if(!(row>>src))return false; int index=source(src); if(index<0) return false;
        auto& slots=sources_[index];
        if(op=="clear") {if(row>>extra) return false;slots.clear();return true;}
        int ttl; if(op!="set"||!(row>>ttl)||ttl<50||ttl>5000) return false;
        std::unordered_map<std::string,float> batch; std::string name; float value;
        while(row>>name) {
            if(!(row>>value)||!std::isfinite(value)||!name_ok(name)||batch.size()>=256||batch.count(name)) return false;
            if(std::none_of(maps_.begin(),maps_.end(),[&](const Mapping& m){return m.input==name;})) return false;
            batch.emplace(name,value);
        }
        if(batch.empty()) return false;
        for(const auto& p:batch) slots[p.first]=Sample{p.second,now+std::chrono::milliseconds(ttl)};
        return true;
    }
    template<class Read,class Write> void apply(Clock::time_point now,float dt,Read read,Write write) {
        dt=std::isfinite(dt)?clamp(dt,0.f,.25f):0.f;
        for(auto& s:sources_) for(auto it=s.begin();it!=s.end();) {if(it->second.expires<=now) it=s.erase(it);else ++it;}
        for(auto& m:maps_) {
            const Sample* sample=nullptr;
            for(int i=2;i>=0;--i) {auto it=sources_[i].find(m.input);if(it!=sources_[i].end()){sample=&it->second;break;}}
            if(!sample){m.active=false;continue;}
            // Honor explicit input/output clamp settings, then apply the rig's own range policy.
            const auto& d=definitions_[m.target];
            float target=mapped_value(d,m,sample->value);
            if(!m.active){m.filtered=bound_value(d,read(m.target));m.active=true;}
            float alpha=m.smoothing_ms>0? -std::expm1(-dt*1000.f/m.smoothing_ms):1.f;
            m.filtered=smooth_value(d,m.filtered,target,alpha);
            write(m.target,bound_value(d,m.filtered));
        }
        // The SDK restores its motion baseline before the late update scheduler.
        // Apply expression layers after tracking, before physics/manual overrides.
        for(auto it=expressions_.begin();it!=expressions_.end();) {
            auto& e=*it;
            if(e.deadline<=now) stop(e,e.deadline);
            e.weight=weight_at(e,now);
            if(e.goal==0&&e.weight<=0){it=expressions_.erase(it);continue;}
            ++it;
        }
        // Compose by blend class, not activation order; overwrite layers crossfade
        // from the preceding layer, and bounds are applied only to the result.
        if(expressions_.empty())return;
        struct Blend {double base=0,mult=1,add=0;bool used=false;};
        std::vector<Blend> blends(definitions_.size());
        for(const auto& e:expressions_)for(const auto& t:e.terms){
            if(e.weight<=0)continue;
            auto& v=blends[t.target];if(!v.used){v.base=read(t.target);v.used=true;}
            const double w=e.weight;
            if(t.blend=='O')v.base+=(t.value-v.base)*w;
            else if(t.blend=='M')v.mult*=1.0+(t.value-1.0)*w;
            else v.add+=t.value*w;
        }
        for(size_t i=0;i<blends.size();++i){const auto& v=blends[i];if(v.used)write(i,bound_value(definitions_[i],v.base*v.mult+v.add));}
    }
};
}
}
