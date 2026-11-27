// Native, optional Twitch integration. Network I/O never runs on the renderer.
#include <curl/curl.h>
#include <curl/websockets.h>
#include <json-c/json.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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
constexpr auto RefreshAge=std::chrono::hours(24*25);
constexpr auto RefreshCooldown=30s;
std::chrono::seconds unix_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
}
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
void atomic(const fs::path& p,const S& s,bool durable=false) {
    if (s.size()>Limit) throw std::runtime_error("File size limit");
    S tmp=(p.parent_path()/".twitch-XXXXXX").string();
    Fd f(mkstemp(tmp.data()));
    if (f.n<0) throw std::runtime_error("Cannot create private file");
    try {
        size_t off=0;
        while (off<s.size()) {
            auto n=write(f.n,s.data()+off,s.size()-off);
            if (n<0&&errno==EINTR) continue;
            if (n<=0) throw std::runtime_error("Cannot write private file");
            off+=n;
        }
        if (fsync(f.n)||rename(tmp.c_str(),p.c_str())) throw std::runtime_error("Cannot publish private file");
    } catch (...) {
        unlink(tmp.c_str());
        throw;
    }
    if (durable) {
        Fd dir(open(p.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        if (dir.n<0||(fsync(dir.n)&&errno!=EINVAL&&errno!=EOPNOTSUPP))
            throw std::runtime_error("Cannot sync private file directory");
    }
}
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
struct TransportError:std::runtime_error{using std::runtime_error::runtime_error;};
struct LoginRequired:std::runtime_error{using std::runtime_error::runtime_error;};
struct RetryAuthentication:std::runtime_error {
    std::chrono::seconds delay;
    explicit RetryAuthentication(const S& message, std::chrono::seconds retry=RefreshCooldown)
        :std::runtime_error(message),delay(retry){}
};
struct HttpError:std::runtime_error {
    long status;
    S reason;
    std::chrono::seconds retry_after;
    HttpError(long s,S r,std::chrono::seconds delay=0s)
        :std::runtime_error("Twitch HTTP "+std::to_string(s)),status(s),reason(std::move(r)),retry_after(delay){}
    bool temporary() const {return status==408||status==429||status>=500;}
    bool rejects_refresh_token() const {
        if (status!=400&&status!=401) return false;
        auto text=reason;
        std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});
        return text=="invalid_grant" || (text.find("refresh token")!=S::npos &&
            (text.find("invalid")!=S::npos||text.find("expired")!=S::npos||text.find("revoked")!=S::npos));
    }
};
std::chrono::seconds retry_delay(CURL* curl) {
    for (const char* name : {"Retry-After", "Ratelimit-Reset"}) {
        curl_header* header = nullptr;
        if (curl_easy_header(curl, name, 0, CURLH_HEADER, -1, &header) != CURLHE_OK) continue;
        try {
            size_t length = 0;
            S text = header->value;
            auto seconds = std::stoll(text, &length);
            if (length != text.size() || seconds < 0) continue;
            if (S(name) == "Ratelimit-Reset") {
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                seconds -= now;
            }
            return std::chrono::seconds(std::clamp(seconds, 1LL, 86400LL));
        } catch (const std::logic_error&) {}
    }
    return 60s;
}
J http(const S& url, const S& body, const std::vector<S>& headers) {
    auto c = handle();
    S out;
    curl_slist* list = nullptr;
    for (const auto& value : headers) list = curl_slist_append(list, value.c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> guard(list, curl_slist_free_all);
    curl_easy_setopt(c.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c.get(), CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &out);
    if (!body.empty()) {
        curl_easy_setopt(c.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c.get(), CURLOPT_POSTFIELDSIZE, long(body.size()));
    }
    auto result = curl_easy_perform(c.get());
    if (result != CURLE_OK) throw TransportError(S("Twitch transport: ") + curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        S reason;
        try {
            auto error=J::parse(out);
            reason=error.text("message");
            if (reason.empty()) reason=error.text("error");
        } catch (const std::exception&) {}
        throw HttpError(status, reason, status == 429 ? retry_delay(c.get()) : 0s);
    }
    return out.empty() ? J::obj() : J::parse(out);
}
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
struct ApprovalRequired:std::runtime_error{using std::runtime_error::runtime_error;};
struct ApiError:std::runtime_error {
    int code;
    explicit ApiError(int error) : std::runtime_error("Kawari rejected a local API request"), code(error) {}
};
struct Api {
    WS ws;
    unsigned seq = 0;
    S approval_request;
    Clock::time_point approval_deadline{};
    explicit Api(unsigned port):ws("ws://127.0.0.1:"+std::to_string(port),true){}
    S send(const S& type, const J& data) {
        auto j = J::obj();
        auto id = std::to_string(++seq);
        j.set("apiName", "VTubeStudioPublicAPI");
        j.set("apiVersion", "1.0");
        j.set("requestID", id);
        j.set("messageType", type + "Request");
        j.set("data", data);
        ws.send(j);
        return id;
    }
    J reply(const J& response, const S& type, const S& id) {
        if (response.text("requestID") != id) throw std::runtime_error("Unexpected Kawari API reply");
        if (response.text("messageType") == "APIError")
            throw ApiError(int(response.get("data").get("errorID").number()));
        if (response.text("messageType") != type + "Response")
            throw std::runtime_error("Unexpected Kawari API response type");
        return response.get("data");
    }
    J call(const S& type, const J& data=J::obj()) {
        auto id = send(type, data);
        return reply(ws.recv(3s), type, id);
    }
    bool authenticate(const S& credential) {
        auto data = identity();
        data.set("authenticationToken", credential);
        return call("Authentication", data).get("authenticated").boolean();
    }
    bool begin_authorization(const fs::path& file) {
        auto credential = load(file).text("token");
        if (!credential.empty() && authenticate(credential)) return true;
        approval_request = send("AuthenticationToken", identity());
        approval_deadline = Clock::now() + 65s;
        return false;
    }
    bool poll_authorization(const fs::path& file) {
        auto response = ws.poll();
        if (!response.p) {
            if (Clock::now() >= approval_deadline) throw ApprovalRequired("Plugin approval expired. Request approval again.");
            return false;
        }
        J data;
        try { data = reply(response, "AuthenticationToken", approval_request); }
        catch (const ApiError& error) {
            if (error.code != 50) throw;
            throw ApprovalRequired("Plugin is not approved. Allow Kawari Twitch in Plugins, then request approval again.");
        }
        auto token = data.text("authenticationToken");
        if (!safe_field(token) || !authenticate(token))
            throw ApprovalRequired("Kawari Twitch authorization failed. Check Plugins and request approval again.");
        auto saved = J::obj();
        saved.set("token", token);
        atomic(file, saved.dump());
        approval_request.clear();
        return true;
    }
};
struct Dedup{std::set<S> seen;std::deque<S> order;S take(const J& m,const S& channel){auto p=m.get("payload"),e=p.get("event");if(m.get("metadata").text("message_type")!="notification"||p.get("subscription").text("type")!=Event||e.text("broadcaster_user_id")!=channel)return {};auto id=e.text("id"),r=e.get("reward").text("id");if(!safe_field(id)||!safe_field(r)||!seen.insert(id).second)return {};order.push_back(id);if(order.size()>4096){seen.erase(order.front());order.pop_front();}return r;}};
bool valid_binding(const J& b){auto m=b.text("mode");auto d=b.get("duration").number(-1);return safe_field(b.text("expressionFile"))&&std::isfinite(d)&&((m=="timed"&&d>0&&d<=300)||((m=="toggle"||m=="on"||m=="off")&&d==0));}
J binding(const S& expression,const S& mode,const S& duration){size_t used=0;auto d=std::stod(duration,&used);if(used!=duration.size()||!std::isfinite(d))throw std::runtime_error("Invalid duration");auto j=J::obj();j.set("expressionFile",expression);j.set("mode",mode);j.set("duration",J::num(mode=="timed"?d:0));if(!valid_binding(j))throw std::runtime_error("Invalid reward binding");return j;}
struct Service {
    using Request = std::function<J(const S&, const S&, const std::vector<S>&)>;
    using OpenSocket = std::function<std::unique_ptr<WS>(const S&)>;
    fs::path mail, root;
    unsigned port;
    Request remote;
    OpenSocket open_event;
    S client, channel, login, model, model_name, status="Disconnected", note, local_note, code, url, device_code;
    J token=J::obj(), rewards, expressions, bindings;
    std::unique_ptr<Api> api, pending_api;
    std::unique_ptr<WS> event, replacement;
    Dedup dedup;
    struct Redemption { S model, reward; Clock::time_point expires; };
    std::deque<Redemption> redemptions;
    S handoff_url, published;
    Clock::time_point retry{}, validate_at{}, rewards_at{}, model_at{}, local_at{}, device_at{}, device_end{}, last_event{};
    Clock::time_point handoff_deadline{}, handoff_at{}, welcome_deadline{};
    Clock::time_point refresh_after{};
    std::chrono::seconds refresh_issued{};
    std::chrono::seconds interval=5s, keepalive=35s, backoff=2s;
    bool enabled=false, requesting_code=false, approval_blocked=false, save_enable_pending=false;
    bool token_dirty=false, refresh_rejected=false;
    pid_t browser=-1;

    Service(fs::path m, fs::path r, unsigned p, Request request=http,
            OpenSocket open=[](const S& address){return std::make_unique<WS>(address);})
        :mail(std::move(m)),root(std::move(r)),port(p),remote(std::move(request)),open_event(std::move(open)) {
        bindings=load(root/"bindings.json");
        auto preferences=load(root/"native-enabled.json");
        try {
            const auto file=root/"oauth.json";
            token=load(file);
            const auto now=unix_time();
            const auto issued=token.get("refresh_issued_at").number();
            if (issued>=1.&&issued<=double(now.count())) {
                refresh_issued=std::chrono::seconds(static_cast<long long>(issued));
            } else if (!token.text("refresh_token").empty()) {
                // Older versions wrote the file only when a new pair was saved.
                struct stat info{};
                if (!lstat(file.c_str(),&info)&&S_ISREG(info.st_mode)&&info.st_mtime>0)
                    refresh_issued=std::min(now,std::chrono::seconds(info.st_mtime));
            }
        }
        catch (const std::exception&) { note="Saved Twitch login could not be read. Connect to sign in again."; }
        client=preferences.text("client_id");
        if (!client_id(client)) client=token.text("client_id");
        if (!client_id(client)) {
            const char* configured=getenv("KAWARI_TWITCH_CLIENT_ID");
            client=configured&&client_id(configured)?configured:"";
        }
        enabled=preferences.get("enabled").boolean() && has_login();
        if (preferences.get("enabled").boolean() && !enabled && note.empty())
            note="Twitch sign-in is required. Click Connect.";
    }

    bool has_login() const {
        return client_id(client)&&token.text("client_id")==client&&
            (safe_field(token.text("access_token"))||safe_field(token.text("refresh_token")));
    }
    void publish() {
        S s="kawari-twitch-1\n";
        auto row=[&](std::initializer_list<S> cols){
            bool first=true;
            for (auto& value:cols) { if(!first)s+='\t';s+=display(value);first=false; }
            s+='\n';
        };
        auto remaining=[](Clock::time_point until){
            return std::to_string(std::max(0s,std::chrono::ceil<std::chrono::seconds>(until-Clock::now())).count());
        };
        row({"S",status,note.empty()&&refresh_rejected
            ?"Twitch could not renew this login. Sign-in will be required when it expires.":note});
        row({"C",client,login,enabled?"1":"0"});
        row({"D",code,url});
        row({"T",device_code.empty()?"":remaining(device_end),status=="Reconnecting"?remaining(retry):""});
        row({"L",local_note,approval_blocked?"1":"0",api?"1":"0"});
        row({"M",model,model_name});
        for (auto& e:expressions.array()) if(safe_field(e.text("file"))) row({"E",e.text("file"),e.text("name")});
        auto b=bindings.get(model);
        std::set<S> listed;
        for (auto& reward:rewards.array()) {
            auto id=reward.text("id");
            if (!safe_field(id)) continue;
            listed.insert(id);
            auto a=b.get(id);
            auto cost=reward.get("cost").number();
            row({"R",id,reward.text("title"),std::to_string(int(std::isfinite(cost)?std::clamp(cost,0.,1000000000.):0.)),
                 a.text("expressionFile"),a.text("mode"),std::to_string(a.get("duration").number())});
        }
        for (auto& id:b.keys()) if(!listed.count(id)&&safe_field(id)) {
            auto a=b.get(id);
            row({"R",id,"Unavailable reward","",a.text("expressionFile"),a.text("mode"),std::to_string(a.get("duration").number())});
        }
        if (s!=published) {atomic(mail/"twitch-ui.tsv",s);published=std::move(s);}
    }
    void preference(bool on) {
        auto j=J::obj();
        j.set("enabled",J(json_object_new_boolean(on)));
        j.set("client_id",client);
        atomic(root/"native-enabled.json",j.dump());
        enabled=on;
    }
    J oauth(const S& endpoint,const S& data={}) {return remote(S(OAuth)+endpoint,data,{});}
    void save_token(J t) {
        if (!safe_field(t.text("access_token")) || !safe_field(t.text("refresh_token")))
            throw std::runtime_error("Invalid OAuth token response");
        refresh_issued=unix_time();
        t.set("client_id",client);
        t.set("refresh_issued_at",J::num(refresh_issued.count()));
        // Twitch rotates device-flow refresh tokens. Keep the new token in memory even
        // if the first disk write fails, then retry that write before another refresh.
        token=std::move(t);
        token_dirty=true;
        refresh_rejected=false;
        persist_token();
    }
    void persist_token() {
        if (!token_dirty) return;
        try {atomic(root/"oauth.json",token.dump(),true);}
        catch (const std::exception&) {throw RetryAuthentication("Could not save the Twitch login. Retrying.");}
        token_dirty=false;
    }
    void refresh() {
        persist_token();
        auto refresh_token=token.text("refresh_token");
        if (refresh_rejected||!safe_field(refresh_token))
            throw LoginRequired("Your Twitch login expired or was revoked. Click Connect to sign in.");
        const auto now=Clock::now();
        if (now<refresh_after)
            throw RetryAuthentication("Waiting to retry Twitch authorization.",std::chrono::ceil<std::chrono::seconds>(refresh_after-now));
        refresh_after=now+RefreshCooldown;
        try {
            save_token(oauth("token",form({{"client_id",client},{"grant_type","refresh_token"},{"refresh_token",refresh_token}})));
        } catch (const HttpError& e) {
            if (e.rejects_refresh_token()) {
                refresh_rejected=true;
                throw LoginRequired("Your Twitch login expired or was revoked. Click Connect to sign in.");
            }
            const auto delay=std::max(RefreshCooldown,e.retry_after);
            refresh_after=Clock::now()+delay;
            throw RetryAuthentication(S(e.what())+" while renewing Twitch authorization. Retrying.",delay);
        } catch (const RetryAuthentication&) {
            refresh_after=Clock::now()+RefreshCooldown;
            throw;
        } catch (const std::exception&) {
            refresh_after=Clock::now()+RefreshCooldown;
            throw RetryAuthentication("Twitch login renewal did not complete. Retrying.");
        }
        refresh_after=Clock::now()+RefreshCooldown;
    }
    template<class Send>
    J authenticated(Send send) {
        const bool needs_refresh=!safe_field(token.text("access_token"));
        if (needs_refresh) refresh();
        try {return send();}
        catch (const HttpError& e) {
            if (e.status!=401) throw;
            if (needs_refresh) throw RetryAuthentication("Twitch did not accept the renewed token yet. Retrying.");
        }
        refresh();
        try {return send();}
        catch (const HttpError& e) {
            if (e.status==401) throw RetryAuthentication("Twitch did not accept the renewed token yet. Retrying.");
            throw;
        }
    }
    J helix(const S& endpoint,const J& data=J()) {
        return authenticated([&](){
            std::vector<S> headers={"Authorization: Bearer "+token.text("access_token"),"Client-Id: "+client};
            if (data.p) headers.push_back("Content-Type: application/json");
            return remote(S(Helix)+endpoint,data.p?data.dump():S(),headers);
        });
    }
    void validate() {
        if (!has_login()) throw LoginRequired("Click Connect to sign in to Twitch.");
        persist_token();
        auto v=authenticated([&](){
            return remote(S(OAuth)+"validate",{},{"Authorization: OAuth "+token.text("access_token")});
        });
        bool scope=false;
        for (auto& field:v.get("scopes").array()) scope|=field.text()==Scope;
        if (!scope||v.text("client_id")!=client||!safe_field(v.text("user_id")))
            throw LoginRequired("Twitch authorization is missing Channel Points access. Click Connect to sign in.");
        if (!channel.empty()&&channel!=v.text("user_id"))
            throw LoginRequired("The Twitch account changed. Click Connect to sign in.");
        channel=v.text("user_id");
        login=v.text("login");
        auto expires=v.get("expires_in").number(1800);
        validate_at=Clock::now()+std::chrono::seconds(int(std::isfinite(expires)?std::clamp(expires,5.,1800.):1800.));
        // Keep an active device grant current even if its access token is still valid.
        if (!refresh_rejected&&refresh_issued>0s&&unix_time()-refresh_issued>=RefreshAge) {
            try {
                refresh();
                validate_at=std::min(validate_at,Clock::now()+RefreshCooldown);
            } catch (const LoginRequired&) {
                refresh_rejected=true;
            } catch (const RetryAuthentication& error) {
                validate_at=std::min(validate_at,Clock::now()+error.delay);
                note=error.what();
            }
        }
    }
    void refresh_rewards() {
        rewards=helix("channel_points/custom_rewards?broadcaster_id="+escape(channel)).get("data");
    }

    void local() {
        if (api||approval_blocked) return;
        const auto credential=root/"kawari-token.json";
        if (!pending_api) {
            pending_api=std::make_unique<Api>(port);
            if (!pending_api->begin_authorization(credential)) {
                local_note="Allow Kawari Twitch in Integrations > Plugins. Twitch can stay connected while you approve it.";
                return;
            }
        } else if (!pending_api->poll_authorization(credential)) {
            return;
        }
        api=std::move(pending_api);
        local_note.clear();
    }
    void clear_model() {model.clear();model_name.clear();expressions=J();}
    bool current() {
        local();
        if (!api) return false;
        auto m=api->call("CurrentModel");
        if (!m.get("modelLoaded").boolean()) {clear_model();return false;}
        auto e=api->call("ExpressionState");
        if (e.text("modelID")!=m.text("modelID")) {clear_model();return false;}
        model=m.text("modelID");
        model_name=m.text("modelName");
        expressions=e.get("expressions");
        return safe_field(model);
    }
    void activate(const S& reward) {
        auto b=bindings.get(model).get(reward);
        if (!b.object()) return;
        if (!valid_binding(b)) throw std::runtime_error("Invalid saved reward binding");
        auto d=J::parse(b.dump());
        d.set("modelID",model);
        api->call("KawariRewardExpression",d);
        note="Reward applied";
    }
    void dispatch(const S& reward,const S& expected={}) {
        if (!current()||(!expected.empty()&&model!=expected)) throw std::runtime_error("Model changed or unavailable");
        activate(reward);
    }
    void process_redemptions() {
        while (!redemptions.empty()&&redemptions.front().expires<=Clock::now()) redemptions.pop_front();
        if (redemptions.empty()||!api) return;
        if (!current()) return;
        auto next=std::move(redemptions.front());
        // An action that may have reached Kawari must not be replayed after a lost reply.
        redemptions.pop_front();
        if (next.model==model&&next.expires>Clock::now()) {
            try {activate(next.reward);}
            catch (const ApiError& e) {
                if(e.code==8)throw;
                note="Kawari could not apply this reward. Check the saved expression.";
            }
        }
    }
    void update_local() {
        if (Clock::now()<local_at||approval_blocked) return;
        try {
            local();
            if (!api) return;
            if (Clock::now()>=model_at) {model_at=Clock::now()+1s;current();}
            process_redemptions();
        } catch (const ApprovalRequired& e) {
            pending_api.reset();
            approval_blocked=true;
            local_note=e.what();
        } catch (const std::exception& e) {
            api.reset();
            pending_api.reset();
            local_at=Clock::now()+2s;
            local_note=S("Reconnecting to Kawari: ")+e.what();
        }
    }

    struct Session { S id; std::chrono::seconds timeout; };
    static Session welcome(const J& m) {
        if (m.get("metadata").text("message_type")!="session_welcome")
            throw std::runtime_error("Expected EventSub welcome");
        auto session=m.get("payload").get("session");
        auto id=session.text("id");
        if (!safe_field(id)) throw std::runtime_error("Missing EventSub session");
        auto duration=session.get("keepalive_timeout_seconds").number(30);
        return {id,std::chrono::seconds(int(std::isfinite(duration)?std::clamp(duration,10.,600.):30.))+5s};
    }
    void connected(std::unique_ptr<WS> socket,const Session& session) {
        event=std::move(socket);
        keepalive=session.timeout;
        last_event=Clock::now();
        status="Connected";
        note.clear();
    }
    void connect_event() {
        status="Connecting";
        publish();
        persist_token();
        if (save_enable_pending) {
            preference(true);
            save_enable_pending=false;
        }
        if (Clock::now()>=validate_at||channel.empty()) validate();
        auto w=open_event(EventWS);
        auto session=welcome(w->recv(10s));
        auto d=J::obj(),condition=J::obj(),transport=J::obj();
        d.set("type",Event);d.set("version","1");
        condition.set("broadcaster_user_id",channel);
        transport.set("method","websocket");transport.set("session_id",session.id);
        d.set("condition",condition);d.set("transport",transport);
        helix("eventsub/subscriptions",d);
        connected(std::move(w),session);
        rewards_at=Clock::now();
    }
    void clear_handoff() {handoff_url.clear();replacement.reset();}
    void start_handoff(const S& address) {
        if (!reconnect_url(address)) throw std::runtime_error("Unexpected EventSub reconnect URL");
        if (!handoff_url.empty()) return;
        handoff_url=address;
        handoff_at=Clock::now();
        handoff_deadline=handoff_at+30s;
        note="Twitch is moving the EventSub connection.";
    }
    void update_handoff() {
        const auto now=Clock::now();
        if (now>=handoff_deadline) throw std::runtime_error("EventSub did not complete the server handoff");
        try {
            if (!replacement) {
                if (now<handoff_at) return;
                replacement=open_event(handoff_url);
                welcome_deadline=Clock::now()+8s;
            }
            auto message=replacement->poll();
            if (message.p) {
                auto session=welcome(message);
                auto socket=std::move(replacement);
                // Drain buffered notifications from the old socket before swapping it.
                poll_events();
                if (enabled) connected(std::move(socket),session);
                clear_handoff();
            } else if (Clock::now()>=welcome_deadline) {
                throw std::runtime_error("Timed out waiting for the new EventSub socket");
            }
        } catch (const std::exception&) {
            replacement.reset();
            handoff_at=Clock::now()+1s;
        }
    }
    void begin_retry(const S& reason,std::chrono::seconds delay=0s) {
        event.reset();
        clear_handoff();
        note=reason;
        status="Reconnecting";
        retry=Clock::now()+std::max(delay,backoff);
        backoff=std::min(60s,backoff*2);
    }
    void event_message(const J& message) {
        last_event=Clock::now();
        auto kind=message.get("metadata").text("message_type");
        if (kind=="session_keepalive"||kind=="notification") backoff=2s;
        if (kind=="session_reconnect") {
            start_handoff(message.get("payload").get("session").text("reconnect_url"));
        } else if (kind=="revocation") {
            auto reason=message.get("payload").get("subscription").text("status");
            if (reason=="authorization_revoked") {
                sign_in_required("Twitch revoked authorization. Click Connect to sign in again.");
            } else {
                disconnect();
                note="Twitch removed this Channel Points subscription. Check your channel and reconnect.";
            }
        } else if (kind=="notification") {
            auto reward=dedup.take(message,channel);
            if (!reward.empty()&&!model.empty()&&bindings.get(model).get(reward).object()) {
                if(redemptions.size()>=64)redemptions.pop_front();
                redemptions.push_back({model,std::move(reward),Clock::now()+20s});
            }
        }
    }

    void start_login() {
        if (!client_id(client)) throw std::runtime_error("Enter a public Twitch Client ID");
        auto d=oauth("device",form({{"client_id",client},{"scopes",Scope}}));
        auto address=d.text("verification_uri"), user_code=d.text("user_code"), secret=d.text("device_code");
        if (!verify_url(address)||!safe_field(user_code)||!safe_field(secret))
            throw std::runtime_error("Invalid device authorization response");
        url=std::move(address);code=std::move(user_code);device_code=std::move(secret);
        auto seconds=d.get("interval").number(5);
        auto expires=d.get("expires_in").number(600);
        interval=std::chrono::seconds(int(std::isfinite(seconds)?std::clamp(seconds,1.,1800.):5.));
        device_end=Clock::now()+std::chrono::seconds(int(std::isfinite(expires)?std::clamp(expires,1.,1800.):600.));
        device_at=Clock::now()+interval;
        status="Waiting for Twitch authorization";
        enabled=true;
    }
    void cancel_device() {device_code.clear();code.clear();url.clear();}
    void delay_login(std::chrono::seconds delay) {
        device_at=Clock::now()+std::max(interval,delay);
        note="Twitch is temporarily unavailable. Retrying the same sign-in code.";
    }
    void poll_login() {
        if (Clock::now()>=device_end) {
            disconnect();
            note="The Twitch code expired. Click Connect for a new code.";
            return;
        }
        if (Clock::now()<device_at) return;
        J credentials;
        try {
            credentials=oauth("token",form({{"client_id",client},{"scopes",Scope},{"device_code",device_code},
                            {"grant_type","urn:ietf:params:oauth:grant-type:device_code"}}));
        } catch (const TransportError&) {delay_login(interval*2);return;}
        catch (const HttpError& e) {
            if (e.reason=="authorization_pending") {device_at=Clock::now()+interval;return;}
            if (e.reason=="slow_down") {interval+=5s;device_at=Clock::now()+interval;return;}
            if (e.temporary()) {delay_login(e.retry_after);return;}
            disconnect();
            note="Twitch did not complete sign-in. Click Connect to get a new code.";
            return;
        }
        // The exchange consumed the one-time device code even if writing the token fails.
        cancel_device();
        status="Connecting";
        note.clear();
        save_enable_pending=true;
        refresh_after={};
        save_token(std::move(credentials));
        preference(true);
        save_enable_pending=false;
        retry={};
    }
    void open_browser() {
        if (!verify_url(url)) return;
        if (browser>0) return;
        browser=fork();
        if (browser==0) {
            execlp("xdg-open","xdg-open",url.c_str(),static_cast<char*>(nullptr));
            _exit(127);
        }
        if (browser<0) note="Could not open the browser. Copy the authorization link.";
    }
    void check_browser() {
        if (browser<=0) return;
        int result=0;
        if (waitpid(browser,&result,WNOHANG)!=browser) return;
        browser=-1;
        if (!device_code.empty()&&(!WIFEXITED(result)||WEXITSTATUS(result)!=0))
            note="Could not open the browser. Copy the authorization link.";
    }
    void disconnect() {
        enabled=false;
        requesting_code=false;
        save_enable_pending=false;
        cancel_device();
        event.reset();clear_handoff();
        api.reset();pending_api.reset();approval_blocked=false;local_note.clear();
        channel.clear();login.clear();clear_model();rewards=J();redemptions.clear();
        validate_at={};local_at={};model_at={};retry={};backoff=2s;
        status="Disconnected";note.clear();
        try {preference(false);}
        catch (const std::exception&) {note="Disconnected. Could not save the Twitch preference.";}
    }
    bool forget_login() {
        std::error_code error;
        fs::remove(root/"oauth.json",error);
        token=J::obj();
        token_dirty=false;
        refresh_rejected=false;
        refresh_issued={};
        refresh_after={};
        return !error;
    }
    void sign_in_required(const S& reason) {
        disconnect();
        status="Sign-in required";
        note=reason;
        if (!forget_login()) note+=" Kawari could not remove the old login file.";
    }
    void request(const std::vector<S>& f) {
        if (f.empty()) return;
        if (f[0]=="connect"&&f.size()==2) {
            auto first=f[1].find_first_not_of(' ');
            auto entered=first==S::npos?S():f[1].substr(first,f[1].find_last_not_of(' ')-first+1);
            if (!client_id(entered)) throw std::runtime_error("Invalid Client ID");
            disconnect();
            client=std::move(entered);
            if (has_login()) preference(true);
            else {
                preference(false);
                enabled=true;
                requesting_code=true;
                status="Requesting a Twitch sign-in code";
            }
        } else if (f[0]=="disconnect"&&f.size()==1) {
            disconnect();
        } else if (f[0]=="logout"&&f.size()==1) {
            disconnect();
            note=forget_login()?"Local OAuth removed":"Could not remove the saved Twitch login.";
        } else if (f[0]=="open"&&f.size()==1&&verify_url(url)) {
            open_browser();
        } else if (f[0]=="approve"&&f.size()==1&&enabled) {
            approval_blocked=false;
            pending_api.reset();
            local_at={};
            local_note.clear();
        } else if (f[0]=="refresh"&&f.size()==1&&event) {
            rewards_at=Clock::now();
            model_at={};
        } else if ((f[0]=="bind"||f[0]=="unbind"||f[0]=="test")&&
                   ((f[0]=="bind"&&f.size()==6)||(f[0]!="bind"&&f.size()==3))) {
            if (!event) throw std::runtime_error("Connect Twitch first");
            if (!current()||model!=f[1]||!safe_field(f[2])) throw std::runtime_error("Model changed; select again");
            if (f[0]=="test") {activate(f[2]);return;}
            auto next=J::parse(bindings.dump());
            auto b=next.get(model);
            if (!b.object()) b=J::obj();
            if (f[0]=="unbind") b.erase(f[2]);
            else {
                bool known=false;
                for (auto& e:expressions.array()) known|=e.text("file")==f[3];
                if (!known) throw std::runtime_error("Unknown expression");
                bool reward_known=b.get(f[2]).object();
                for (auto& r:rewards.array()) reward_known|=r.text("id")==f[2];
                if (!reward_known) throw std::runtime_error("Unknown reward");
                b.set(f[2],binding(f[3],f[4],f[5]));
            }
            next.set(model,b);
            atomic(root/"bindings.json",next.dump());
            bindings=std::move(next);
            note="Binding saved";
        } else {
            throw std::runtime_error("Invalid Twitch command");
        }
    }
    void poll_request() {
        auto req=mail/"twitch-request.tsv";
        if (!fs::exists(req)) return;
        bool consumed=false;
        try {
            auto s=read_private(req);
            fs::remove(req);
            consumed=true;
            if (!s.empty()&&s.back()=='\n') s.pop_back();
            if (s.find_first_of("\r\n")!=S::npos) throw std::runtime_error("Invalid request");
            request(fields(s));
        } catch (const std::exception& e) {
            if (!consumed) {
                std::error_code ignored;
                fs::remove(req,ignored);
            }
            note=e.what();
        }
        publish();
    }
    void poll_events() {
        for (int i=0;event&&i<8;++i) {
            J message;
            try {message=event->poll();}
            catch (const std::exception&) {
                if (handoff_url.empty()) throw;
                event.reset();
                break;
            }
            if (!message.p) break;
            event_message(message);
        }
    }
    void update_remote() {
        if (!event&&handoff_url.empty()) {
            if (Clock::now()>=retry) connect_event();
            return;
        }
        poll_events();
        if (!enabled) return;
        if (!handoff_url.empty()) {update_handoff();return;}
        if (!event) return;
        if (Clock::now()-last_event>keepalive) throw std::runtime_error("EventSub keepalive timed out");
        if (Clock::now()>=validate_at) {
            try {validate();}
            catch (const LoginRequired&) {throw;}
            catch (const RetryAuthentication& e) {
                validate_at=Clock::now()+e.delay;
                note=e.what();
            }
            catch (const HttpError& e) {
                if (!e.temporary()) throw;
                validate_at=Clock::now()+std::max(15s,e.retry_after);
                note="Twitch could not validate the login yet. Retrying.";
            } catch (const TransportError&) {
                validate_at=Clock::now()+15s;
                note="Twitch could not validate the login yet. Retrying.";
            }
        }
        if (Clock::now()>=rewards_at) {
            try {refresh_rewards();rewards_at=Clock::now()+5min;}
            catch (const LoginRequired&) {throw;}
            catch (const RetryAuthentication& e) {
                rewards_at=Clock::now()+e.delay;
                note=e.what();
            }
            catch (const HttpError& e) {
                rewards_at=Clock::now()+std::max(60s,e.retry_after);
                note=e.status==403?"Twitch did not grant access to this channel's rewards.":"Could not refresh Channel Points rewards.";
            } catch (const std::exception&) {
                rewards_at=Clock::now()+60s;
                note="Could not refresh Channel Points rewards.";
            }
        }
    }
    void tick() {
        check_browser();
        poll_request();
        if (!enabled) return;
        try {
            if (requesting_code) {
                if (Clock::now()>=retry) {
                    start_login();
                    requesting_code=false;
                    open_browser();
                }
                return;
            }
            if (!device_code.empty()) {poll_login();return;}
            update_remote();
        } catch (const LoginRequired& e) {
            sign_in_required(e.what());
        } catch (const RetryAuthentication& e) {
            begin_retry(e.what(),e.delay);
        } catch (const HttpError& e) {
            if (!e.temporary()) {
                const bool sign_in=requesting_code;
                disconnect();
                if (sign_in) note="Twitch rejected sign-in. Check the Client ID and confirm the app is Public.";
                else if (e.status==403) note="Twitch refused the Channel Points subscription. Check channel eligibility and app permissions.";
                else note=S(e.what())+". Check the Twitch app and reconnect.";
            } else {
                begin_retry(e.status==429?"Twitch temporarily limited requests.":e.what(),e.retry_after);
            }
        } catch (const std::exception& e) {
            if (!device_code.empty()) {disconnect();note=e.what();}
            else begin_retry(e.what());
        }
        if (enabled&&device_code.empty()&&!channel.empty()) update_local();
    }
    void run() {
        publish();
        auto next=Clock::now();
        while (fs::is_directory(mail)) {
            tick();
            if (Clock::now()>=next) {publish();next=Clock::now()+250ms;}
            std::this_thread::sleep_for(30ms);
        }
    }
};
int main(int argc,char** argv){try{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("libcurl initialization failed");if(argc==2&&S(argv[1])=="--check"){bool ws=false,wss=false;for(auto p=curl_version_info(CURLVERSION_NOW)->protocols;*p;++p){ws|=S(*p)=="ws";wss|=S(*p)=="wss";}if(!ws||!wss)throw std::runtime_error("libcurl must support ws and wss");std::cout<<"Kawari native Twitch protocol 1\n";return 0;}if(argc!=4)throw std::runtime_error("Usage: kawari-twitch MAILBOX DATA_DIR PORT");umask(077);fs::path mail=argv[1],root=fs::path(argv[2])/"twitch";private_dir(mail);fs::create_directories(root);private_dir(root);Fd lock(open((root/"bridge.lock").c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600));if(lock.n<0||flock(lock.n,LOCK_EX|LOCK_NB))throw std::runtime_error("Another Twitch bridge is running");unsigned port=std::stoul(argv[3]);if(!port||port>65535)throw std::runtime_error("Invalid API port");Service service(mail,root,port);service.run();return 0;}catch(const std::exception& e){std::cerr<<"[twitch] "<<e.what()<<'\n';return 1;}}
