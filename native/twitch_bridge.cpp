// Native, optional Twitch integration. Network I/O never runs on the renderer.
#include <curl/curl.h>
#include <curl/websockets.h>
#include <json-c/json.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
using S=std::string;
constexpr size_t Limit=1024*1024;
constexpr auto Scope="channel:read:redemptions";
constexpr auto Event="channel.channel_points_custom_reward_redemption.add";
constexpr auto OAuth="https://id.twitch.tv/oauth2/";
constexpr auto Helix="https://api.twitch.tv/helix/";
constexpr auto EventWS="wss://eventsub.wss.twitch.tv/ws";
struct J {
    std::shared_ptr<json_object> p;
    explicit J(json_object* o=nullptr):p(o,[](json_object* x){if(x)json_object_put(x);}){}
    static J obj(){return J(json_object_new_object());}
    static J str(const S& s){return J(json_object_new_string_len(s.data(),s.size()));}
    static J num(double n){return J(json_object_new_double(n));}
    static J parse(const S& s){auto* t=json_tokener_new();auto* o=json_tokener_parse_ex(t,s.data(),s.size());auto e=json_tokener_get_error(t);auto end=json_tokener_get_parse_end(t);json_tokener_free(t);if(e!=json_tokener_success||s.find_first_not_of(" \r\n\t",end)!=S::npos){if(o)json_object_put(o);throw std::runtime_error("Invalid JSON response");}return J(o);}
    J get(const S& k)const{json_object* v=nullptr;if(p&&json_object_object_get_ex(p.get(),k.c_str(),&v))return J(json_object_get(v));return J();}
    S text()const{return p&&json_object_is_type(p.get(),json_type_string)?S(json_object_get_string(p.get()),json_object_get_string_len(p.get())):S();}
    S text(const S& k)const{return get(k).text();}
    double number(double d=0)const{return p&&(json_object_is_type(p.get(),json_type_int)||json_object_is_type(p.get(),json_type_double))?json_object_get_double(p.get()):d;}
    bool boolean()const{return p&&json_object_is_type(p.get(),json_type_boolean)&&json_object_get_boolean(p.get());}
    bool object()const{return p&&json_object_is_type(p.get(),json_type_object);}
    void set(const S& k,const J& v){if(!object())throw std::runtime_error("Expected object");json_object_object_add(p.get(),k.c_str(),json_object_get(v.p.get()));}
    void set(const S& k,const S& v){set(k,str(v));}
    void erase(const S& k){if(object())json_object_object_del(p.get(),k.c_str());}
    std::vector<J> array()const{std::vector<J> a;if(p&&json_object_is_type(p.get(),json_type_array))for(size_t i=0;i<json_object_array_length(p.get());++i)a.emplace_back(json_object_get(json_object_array_get_idx(p.get(),i)));return a;}
    std::vector<S> keys()const{std::vector<S> a;if(object()){json_object_object_foreach(p.get(),k,v){(void)v;a.emplace_back(k);}}return a;}
    S dump()const{return p?json_object_to_json_string_ext(p.get(),JSON_C_TO_STRING_PLAIN):"null";}
};
struct Fd{int n;explicit Fd(int v):n(v){}~Fd(){if(n>=0)close(n);}Fd(const Fd&)=delete;};
void private_dir(const fs::path& p){struct stat st{};if(lstat(p.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077))throw std::runtime_error("Expected private Twitch directory (0700)");}
S read_private(const fs::path& p){Fd f(open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));if(f.n<0){if(errno==ENOENT)return {};throw std::runtime_error("Cannot read private file");}struct stat st{};if(fstat(f.n,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)||st.st_size>long(Limit))throw std::runtime_error("Expected private file (0600)");S s;char b[4096];for(;;){auto n=read(f.n,b,sizeof(b));if(n<0&&errno==EINTR)continue;if(n<0||s.size()+size_t(n)>Limit)throw std::runtime_error("Cannot read private file");if(!n)break;s.append(b,n);}return s;}
J load(const fs::path& p){auto s=read_private(p);if(s.empty()){if(fs::exists(p))throw std::runtime_error("Empty private JSON file");return J::obj();}auto j=J::parse(s);if(!j.object())throw std::runtime_error("Invalid private JSON file");return j;}
void atomic(const fs::path& p,const S& s){if(s.size()>Limit)throw std::runtime_error("File size limit");S tmp=(p.parent_path()/".twitch-XXXXXX").string();Fd f(mkstemp(tmp.data()));if(f.n<0)throw std::runtime_error("Cannot create private file");try{size_t off=0;while(off<s.size()){auto n=write(f.n,s.data()+off,s.size()-off);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("Cannot write private file");off+=n;}if(fsync(f.n)||rename(tmp.c_str(),p.c_str()))throw std::runtime_error("Cannot publish private file");}catch(...){unlink(tmp.c_str());throw;}}
bool safe_field(const S& s){return !s.empty()&&s.size()<=4096&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;});}
S display(S s){for(char& c:s)if(static_cast<unsigned char>(c)<32||c==127)c=' ';return s.substr(0,4096);}
bool client_id(const S& s){return s.size()>=10&&s.size()<=128&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');});}
bool url_host(const S& s,const S& origin){return s==origin||s.rfind(origin+"/",0)==0||s.rfind(origin+"?",0)==0;}
bool verify_url(const S& s){return safe_field(s)&&(url_host(s,"https://www.twitch.tv")||url_host(s,"https://twitch.tv")||url_host(s,"https://id.twitch.tv"));}
bool reconnect_url(const S& s){return safe_field(s)&&url_host(s,"wss://eventsub.wss.twitch.tv");}
std::vector<S> fields(const S& s){std::vector<S> v;size_t a=0;for(;;){auto b=s.find('\t',a);v.push_back(s.substr(a,b-a));if(b==S::npos)break;a=b+1;}return v;}
struct CurlDel{void operator()(CURL* c)const{curl_easy_cleanup(c);}};
using Curl=std::unique_ptr<CURL,CurlDel>;
Curl handle(){Curl c(curl_easy_init());if(!c)throw std::runtime_error("Cannot initialize libcurl");curl_easy_setopt(c.get(),CURLOPT_NOSIGNAL,1L);curl_easy_setopt(c.get(),CURLOPT_CONNECTTIMEOUT_MS,5000L);curl_easy_setopt(c.get(),CURLOPT_TIMEOUT_MS,12000L);curl_easy_setopt(c.get(),CURLOPT_FOLLOWLOCATION,0L);return c;}
S escape(const S& s){auto c=handle();char* p=curl_easy_escape(c.get(),s.data(),s.size());if(!p)throw std::runtime_error("Cannot encode request");S out=p;curl_free(p);return out;}
S form(std::initializer_list<std::pair<S,S>> args){S r;for(auto& [k,v]:args){if(!r.empty())r+='&';r+=escape(k)+"="+escape(v);}return r;}
size_t append(char* p,size_t a,size_t b,void* user){auto& s=*static_cast<S*>(user);if(a&&b>Limit/a)return 0;auto n=a*b;if(n>Limit-s.size())return 0;s.append(p,n);return n;}
struct HttpError:std::runtime_error{long status;S reason;HttpError(long s,S r):std::runtime_error("Twitch HTTP "+std::to_string(s)),status(s),reason(std::move(r)){} };
J http(const S& url,const S& body={},const std::vector<S>& headers={}){auto c=handle();S out;curl_slist* list=nullptr;for(const auto& h:headers)list=curl_slist_append(list,h.c_str());std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> guard(list,curl_slist_free_all);curl_easy_setopt(c.get(),CURLOPT_URL,url.c_str());curl_easy_setopt(c.get(),CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(c.get(),CURLOPT_HTTPHEADER,list);curl_easy_setopt(c.get(),CURLOPT_WRITEFUNCTION,append);curl_easy_setopt(c.get(),CURLOPT_WRITEDATA,&out);if(!body.empty()){curl_easy_setopt(c.get(),CURLOPT_POSTFIELDS,body.data());curl_easy_setopt(c.get(),CURLOPT_POSTFIELDSIZE,long(body.size()));}auto r=curl_easy_perform(c.get());if(r!=CURLE_OK)throw std::runtime_error(S("Twitch transport: ")+curl_easy_strerror(r));long status=0;curl_easy_getinfo(c.get(),CURLINFO_RESPONSE_CODE,&status);if(status<200||status>=300){S reason;try{reason=J::parse(out).text("message");}catch(...){}throw HttpError(status,reason);}return out.empty()?J::obj():J::parse(out);}
struct WS {
    Curl c;
    S pending;
    explicit WS(const S& url,bool local=false):c(handle()){
        curl_easy_setopt(c.get(),CURLOPT_URL,url.c_str());curl_easy_setopt(c.get(),CURLOPT_PROTOCOLS_STR,local?"ws":"wss");if(local)curl_easy_setopt(c.get(),CURLOPT_PROXY,"");curl_easy_setopt(c.get(),CURLOPT_CONNECT_ONLY,2L);
        auto r=curl_easy_perform(c.get());if(r!=CURLE_OK)throw std::runtime_error(S(local?"Local API: ":"EventSub: ")+curl_easy_strerror(r));
    }
    ~WS(){if(c){size_t n=0;(void)curl_ws_send(c.get(),"",0,&n,0,CURLWS_CLOSE);}}
    // null means there is currently no complete message, not a disconnection.
    J poll(){char b[8192];for(int i=0;i<64;++i){size_t n=0;const curl_ws_frame* meta=nullptr;auto r=curl_ws_recv(c.get(),b,sizeof(b),&n,&meta);if(r==CURLE_AGAIN)return J();if(r!=CURLE_OK||!meta)throw std::runtime_error("WebSocket connection lost");if(meta->flags&CURLWS_CLOSE)throw std::runtime_error("WebSocket closed");if(meta->flags&(CURLWS_PING|CURLWS_PONG))continue;if(n>Limit-pending.size())throw std::runtime_error("WebSocket message too large");pending.append(b,n);if(meta->bytesleft==0&&!(meta->flags&CURLWS_CONT)){auto s=std::move(pending);pending.clear();return J::parse(s);}}return J();}
    J recv(std::chrono::milliseconds timeout){auto end=Clock::now()+timeout;while(Clock::now()<end){auto j=poll();if(j.p)return j;std::this_thread::sleep_for(15ms);}throw std::runtime_error("WebSocket response timed out");}
    void send(const J& j){S s=j.dump();size_t sent=0;auto end=Clock::now()+5s;for(;;){size_t n=0;auto r=curl_ws_send(c.get(),s.data()+sent,s.size()-sent,&n,0,CURLWS_TEXT);sent+=n;if(r==CURLE_OK&&sent==s.size())return;if(r!=CURLE_AGAIN&&r!=CURLE_OK)throw std::runtime_error("WebSocket send failed");if(Clock::now()>=end)throw std::runtime_error("WebSocket send timed out");std::this_thread::sleep_for(15ms);}}
};
J identity(){auto j=J::obj();j.set("pluginName","Kawari Twitch");j.set("pluginDeveloper","Kawari");return j;}
struct Api {
    WS ws;unsigned seq=0;
    explicit Api(unsigned port):ws("ws://127.0.0.1:"+std::to_string(port),true){}
    J call(const S& type,const J& data=J::obj(),std::chrono::milliseconds timeout=5s){auto j=J::obj();S id=std::to_string(++seq);j.set("apiName","VTubeStudioPublicAPI");j.set("apiVersion","1.0");j.set("requestID",id);j.set("messageType",type+"Request");j.set("data",data);ws.send(j);auto r=ws.recv(timeout);if(r.text("requestID")!=id||r.text("messageType")!=type+"Response")throw std::runtime_error("Local API rejected "+type);return r.get("data");}
    void authorize(const fs::path& file){auto d=identity();auto tok=load(file).text("token");if(!tok.empty()){d.set("authenticationToken",tok);if(call("Authentication",d).get("authenticated").boolean())return;}auto newtok=call("AuthenticationToken",identity(),65s).text("authenticationToken");if(newtok.empty())throw std::runtime_error("Approve Kawari Twitch in Plugins");d.set("authenticationToken",newtok);if(!call("Authentication",d).get("authenticated").boolean())throw std::runtime_error("Local API authorization failed");auto saved=J::obj();saved.set("token",newtok);atomic(file,saved.dump());}
};
struct Dedup{std::set<S> seen;std::deque<S> order;S take(const J& m,const S& channel){auto p=m.get("payload"),e=p.get("event");if(m.get("metadata").text("message_type")!="notification"||p.get("subscription").text("type")!=Event||e.text("broadcaster_user_id")!=channel)return {};auto id=e.text("id"),r=e.get("reward").text("id");if(!safe_field(id)||!safe_field(r)||!seen.insert(id).second)return {};order.push_back(id);if(order.size()>4096){seen.erase(order.front());order.pop_front();}return r;}};
bool valid_binding(const J& b){auto m=b.text("mode");auto d=b.get("duration").number(-1);return safe_field(b.text("expressionFile"))&&std::isfinite(d)&&((m=="timed"&&d>0&&d<=300)||((m=="toggle"||m=="on"||m=="off")&&d==0));}
J binding(const S& expression,const S& mode,const S& duration){size_t used=0;auto d=std::stod(duration,&used);if(used!=duration.size()||!std::isfinite(d))throw std::runtime_error("Invalid duration");auto j=J::obj();j.set("expressionFile",expression);j.set("mode",mode);j.set("duration",J::num(mode=="timed"?d:0));if(!valid_binding(j))throw std::runtime_error("Invalid reward binding");return j;}
struct Service {
    fs::path mail,root;unsigned port;
    S client,channel,login,model,model_name,status="Disconnected",note,code,url,device_code;
    J token=J::obj(),rewards,expressions,bindings;
    std::unique_ptr<Api> api;std::unique_ptr<WS> event;Dedup dedup;
    S published;
    Clock::time_point retry{},validate_at{},model_at{},device_at{},device_end{},last_event{};int interval=5,keepalive=40,backoff=5;bool enabled=false;
    Service(fs::path m,fs::path r,unsigned p):mail(std::move(m)),root(std::move(r)),port(p){bindings=load(root/"bindings.json");token=load(root/"oauth.json");client=token.text("client_id");if(!client_id(client)){const char* e=getenv("KAWARI_TWITCH_CLIENT_ID");client=e&&client_id(e)?e:"";}enabled=load(root/"native-enabled.json").get("enabled").boolean();if(enabled&&!client_id(client))enabled=false;}
    void publish(){S s="kawari-twitch-1\n";auto row=[&](std::initializer_list<S> cols){bool first=true;for(auto& c:cols){if(!first)s+='\t';s+=display(c);first=false;}s+='\n';};row({"S",status,note});row({"C",client,login,enabled?"1":"0"});row({"D",code,url});row({"M",model,model_name});for(auto& e:expressions.array())if(safe_field(e.text("file")))row({"E",e.text("file"),e.text("name")});auto b=bindings.get(model);std::set<S> listed;for(auto& r:rewards.array()){auto id=r.text("id");if(!safe_field(id))continue;listed.insert(id);auto a=b.get(id);row({"R",id,r.text("title"),std::to_string(int(std::clamp(r.get("cost").number(),0.,1000000000.))),a.text("expressionFile"),a.text("mode"),std::to_string(a.get("duration").number())});}for(auto& id:b.keys())if(!listed.count(id)&&safe_field(id)){auto a=b.get(id);row({"R",id,"Unavailable reward","",a.text("expressionFile"),a.text("mode"),std::to_string(a.get("duration").number())});}if(s!=published){atomic(mail/"twitch-ui.tsv",s);published=std::move(s);}}
    void preference(bool on){auto j=J::obj();j.set("enabled",J(json_object_new_boolean(on)));atomic(root/"native-enabled.json",j.dump());enabled=on;}
    J oauth(const S& endpoint,const S& data={}){return http(S(OAuth)+endpoint,data);}
    J helix(const S& endpoint,const J& data=J()){std::vector<S> h={"Authorization: Bearer "+token.text("access_token"),"Client-Id: "+client};if(data.p)h.push_back("Content-Type: application/json");return http(S(Helix)+endpoint,data.p?data.dump():S(),h);}
    void save_token(J t){if(!safe_field(t.text("access_token"))||!safe_field(t.text("refresh_token")))throw std::runtime_error("Invalid OAuth token response");t.set("client_id",client);atomic(root/"oauth.json",t.dump());token=std::move(t);}
    void refresh(){auto r=token.text("refresh_token");if(!safe_field(r))throw std::runtime_error("Login required");save_token(oauth("token",form({{"client_id",client},{"grant_type","refresh_token"},{"refresh_token",r}})));}
    void validate(){auto check=[&](){return http(S(OAuth)+"validate",{}, {"Authorization: OAuth "+token.text("access_token")});};if(token.text("client_id")!=client||!safe_field(token.text("access_token")))throw std::runtime_error("Login required");J v;try{v=check();}catch(const HttpError& e){if(e.status!=401)throw;refresh();v=check();}if(v.get("expires_in").number()<120){refresh();v=check();}bool scope=false;for(auto& s:v.get("scopes").array())scope|=s.text()==Scope;if(!scope||v.text("client_id")!=client||!safe_field(v.text("user_id")))throw std::runtime_error("Wrong account scope; log in again");if(!channel.empty()&&channel!=v.text("user_id"))throw std::runtime_error("Twitch account changed; disconnect first");channel=v.text("user_id");login=v.text("login");validate_at=Clock::now()+std::chrono::seconds(int(std::clamp(v.get("expires_in").number()-120,30.,1800.)));}
    void refresh_rewards(){rewards=helix("channel_points/custom_rewards?broadcaster_id="+escape(channel)).get("data");}
    void local(){if(api)return;const auto previous=status;status="Authorizing local API";note="Approve Kawari Twitch in Integrations > Plugins if requested";publish();auto a=std::make_unique<Api>(port);a->authorize(root/"kawari-token.json");api=std::move(a);status=previous;note.clear();}
    bool current(){local();auto m=api->call("CurrentModel");if(!m.get("modelLoaded").boolean()){model.clear();model_name.clear();expressions=J();return false;}model=m.text("modelID");model_name=m.text("modelName");auto e=api->call("ExpressionState");if(e.text("modelID")!=model){model.clear();model_name.clear();expressions=J();return false;}expressions=e.get("expressions");return safe_field(model);}
    void dispatch(const S& reward,const S& expected={}){if(!current()||(!expected.empty()&&model!=expected))throw std::runtime_error("Model changed or unavailable");auto b=bindings.get(model).get(reward);if(!b.object())return;if(!valid_binding(b))throw std::runtime_error("Invalid saved reward binding");auto d=J::parse(b.dump());d.set("modelID",model);api->call("KawariRewardExpression",d);note="Reward applied";}
    void welcome(WS& w){auto m=w.recv(10s);if(m.get("metadata").text("message_type")!="session_welcome")throw std::runtime_error("Expected EventSub welcome");auto s=m.get("payload").get("session");keepalive=int(std::clamp(s.get("keepalive_timeout_seconds").number(30),10.,600.))+10;last_event=Clock::now();session=s.text("id");if(!safe_field(session))throw std::runtime_error("Missing EventSub session");}
    S session;
    void connect_event(){status="Connecting";publish();validate();local();current();auto w=std::make_unique<WS>(EventWS);welcome(*w);auto d=J::obj(),condition=J::obj(),transport=J::obj();d.set("type",Event);d.set("version","1");condition.set("broadcaster_user_id",channel);transport.set("method","websocket");transport.set("session_id",session);d.set("condition",condition);d.set("transport",transport);helix("eventsub/subscriptions",d);event=std::move(w);status="Connected";note.clear();try{refresh_rewards();}catch(const std::exception& e){note=e.what();}}
    void start_login(){if(!client_id(client))throw std::runtime_error("Enter Twitch Client ID");event.reset();api.reset();channel.clear();login.clear();rewards=J();auto d=oauth("device",form({{"client_id",client},{"scopes",Scope}}));url=d.text("verification_uri");code=d.text("user_code");device_code=d.text("device_code");if(!verify_url(url)||!safe_field(code)||!safe_field(device_code)){code.clear();url.clear();device_code.clear();throw std::runtime_error("Invalid device authorization response");}interval=int(std::clamp(d.get("interval").number(5),5.,60.));device_end=Clock::now()+std::chrono::seconds(int(std::clamp(d.get("expires_in").number(600),1.,1800.)));device_at=Clock::now()+std::chrono::seconds(interval);status="Waiting for Twitch authorization";}
    void cancel_device(){device_code.clear();code.clear();url.clear();}
    void poll_login(){if(Clock::now()>=device_end){cancel_device();preference(false);status="Disconnected";note="Authorization expired";return;}if(Clock::now()<device_at)return;device_at=Clock::now()+std::chrono::seconds(interval);try{save_token(oauth("token",form({{"client_id",client},{"scopes",Scope},{"device_code",device_code},{"grant_type","urn:ietf:params:oauth:grant-type:device_code"}})));cancel_device();preference(true);retry={};status="Connecting";}catch(const HttpError& e){if(e.reason=="authorization_pending")return;if(e.reason=="slow_down"){interval+=5;device_at=Clock::now()+std::chrono::seconds(interval);return;}cancel_device();preference(false);status="Disconnected";note=e.what();}}
    void disconnect(){preference(false);cancel_device();event.reset();api.reset();channel.clear();login.clear();model.clear();model_name.clear();expressions=J();rewards=J();status="Disconnected";note.clear();}
    void request(const std::vector<S>& f){if(f.empty())return;note.clear();if(f[0]=="connect"&&f.size()==2){if(!client_id(f[1]))throw std::runtime_error("Invalid Client ID");if(enabled)disconnect();client=f[1];retry={};backoff=5;if(token.text("client_id")!=client||token.text("access_token").empty()){preference(false);start_login();enabled=true;}else preference(true);}
        else if(f[0]=="disconnect"&&f.size()==1)disconnect();
        else if(f[0]=="logout"&&f.size()==1){disconnect();fs::remove(root/"oauth.json");token=J::obj();note="Local OAuth removed; revoke app in Twitch settings if desired";}
        else if(f[0]=="open"&&f.size()==1&&verify_url(url)){if(pid_t p=fork();p==0){execlp("xdg-open","xdg-open",url.c_str(),static_cast<char*>(nullptr));_exit(127);}else if(p<0)throw std::runtime_error("Cannot open browser");}
        else if(f[0]=="refresh"&&f.size()==1&&event){refresh_rewards();current();}
        else if((f[0]=="bind"||f[0]=="unbind"||f[0]=="test")&&((f[0]=="bind"&&f.size()==6)||(f[0]!="bind"&&f.size()==3))){if(!event)throw std::runtime_error("Connect Twitch first");if(!current()||model!=f[1]||!safe_field(f[2]))throw std::runtime_error("Model changed; select again");auto next=J::parse(bindings.dump());auto b=next.get(model);if(!b.object())b=J::obj();if(f[0]=="test"){dispatch(f[2],f[1]);return;}if(f[0]=="unbind")b.erase(f[2]);else{bool known=false;for(auto& e:expressions.array())known|=e.text("file")==f[3];if(!known)throw std::runtime_error("Unknown expression");bool reward_known=b.get(f[2]).object();for(auto& r:rewards.array())reward_known|=r.text("id")==f[2];if(!reward_known)throw std::runtime_error("Unknown reward");b.set(f[2],binding(f[3],f[4],f[5]));}next.set(model,b);atomic(root/"bindings.json",next.dump());bindings=std::move(next);note="Binding saved";}
        else throw std::runtime_error("Invalid Twitch command");}
    void tick(){int ignored=0;while(waitpid(-1,&ignored,WNOHANG)>0){}auto req=mail/"twitch-request.tsv";if(fs::exists(req)){try{auto s=read_private(req);fs::remove(req);if(!s.empty()&&s.back()=='\n')s.pop_back();if(s.find_first_of("\r\n")!=S::npos)throw std::runtime_error("Invalid request");request(fields(s));}catch(const std::exception& e){fs::remove(req);note=e.what();if(status=="Disconnected"||status=="Waiting for Twitch authorization"){if(device_code.empty())preference(false);}}publish();}
        if(!enabled)return;
        if(!device_code.empty()){poll_login();return;}
        if(!event){if(Clock::now()<retry)return;connect_event();return;}
        auto m=event->poll();if(m.p){last_event=Clock::now();backoff=5;auto kind=m.get("metadata").text("message_type");if(kind=="session_reconnect"){auto u=m.get("payload").get("session").text("reconnect_url");if(!reconnect_url(u))throw std::runtime_error("Unexpected reconnect URL");auto w=std::make_unique<WS>(u);welcome(*w);event=std::move(w);}else if(kind=="revocation"){disconnect();note="Twitch revoked subscription; connect again";}else if(kind=="notification"){auto r=dedup.take(m,channel);if(!r.empty()){try{dispatch(r);}catch(const std::exception& e){note=e.what();}}}}
        if(!event)return;
        if(Clock::now()-last_event>std::chrono::seconds(keepalive))throw std::runtime_error("EventSub keepalive timeout");
        if(Clock::now()>=validate_at)validate();
        if(Clock::now()>=model_at){model_at=Clock::now()+1s;current();}}
    void run(){publish();auto next=Clock::now();while(fs::is_directory(mail)){try{tick();}catch(const HttpError& e){event.reset();api.reset();note=e.what();if(e.status==429)backoff=60;if(e.status==400||e.status==401||e.status==403){preference(false);cancel_device();status="Disconnected";}else{status="Reconnecting";retry=Clock::now()+std::chrono::seconds(backoff);backoff=std::min(60,backoff*2);}}catch(const std::exception& e){event.reset();api.reset();note=e.what();if(!device_code.empty()||note=="Login required"||note.find("Local API rejected ")==0||note=="Approve Kawari Twitch in Plugins"||note=="Local API authorization failed"){preference(false);cancel_device();status="Disconnected";}else{status="Reconnecting";retry=Clock::now()+std::chrono::seconds(backoff);backoff=std::min(60,backoff*2);}}if(Clock::now()>=next){publish();next=Clock::now()+250ms;}std::this_thread::sleep_for(30ms);}}
};
int main(int argc,char** argv){try{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("libcurl initialization failed");if(argc==2&&S(argv[1])=="--check"){bool ws=false,wss=false;for(auto p=curl_version_info(CURLVERSION_NOW)->protocols;*p;++p){ws|=S(*p)=="ws";wss|=S(*p)=="wss";}if(!ws||!wss)throw std::runtime_error("libcurl must support ws and wss");std::cout<<"Kawari native Twitch protocol 1\n";return 0;}if(argc!=4)throw std::runtime_error("Usage: kawari-twitch MAILBOX DATA_DIR PORT");umask(077);fs::path mail=argv[1],root=fs::path(argv[2])/"twitch";private_dir(mail);fs::create_directories(root);private_dir(root);Fd lock(open((root/"bridge.lock").c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600));if(lock.n<0||flock(lock.n,LOCK_EX|LOCK_NB))throw std::runtime_error("Another Twitch bridge is running");unsigned port=std::stoul(argv[3]);if(!port||port>65535)throw std::runtime_error("Invalid API port");Service service(mail,root,port);service.run();return 0;}catch(const std::exception& e){std::cerr<<"[twitch] "<<e.what()<<'\n';return 1;}}
