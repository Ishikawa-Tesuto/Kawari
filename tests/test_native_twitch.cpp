#define main kawari_twitch_main
#include "../native/twitch_bridge.cpp"
#undef main
#include <cassert>

void test_device_login(const fs::path& root, unsigned port) {
    fs::create_directory(root);
    int attempts=0;
    auto request=[&](const S& url,const S& body,const std::vector<S>& headers) -> J {
        if (url==S(OAuth)+"device")
            return J::parse(R"({"user_code":"CODE","device_code":"device-secret","verification_uri":"https://www.twitch.tv/activate","interval":5,"expires_in":600})");
        if (url==S(OAuth)+"token"&&body.find("grant_type=refresh_token")!=S::npos) {
            assert(body.find("refresh_token=old-refresh")!=S::npos);
            return J::parse(R"({"access_token":"new-access","refresh_token":"new-refresh"})");
        }
        if (url==S(OAuth)+"token") {
            assert(body.find("device_code=device-secret")!=S::npos);
            switch (++attempts) {
                case 1: throw HttpError(400,"authorization_pending");
                case 2: throw TransportError("mock network outage");
                case 3: throw HttpError(400,"slow_down");
                case 4: throw HttpError(429,"rate limited",35s);
                default: return J::parse(R"({"access_token":"old-access","refresh_token":"old-refresh"})");
            }
        }
        assert(url==S(Helix)+"test");
        if (headers.front()=="Authorization: Bearer old-access") throw HttpError(401,"expired");
        assert(headers.front()=="Authorization: Bearer new-access");
        return J::obj();
    };
    Service s(root,root,port,request);
    s.client="client123456789";
    s.preference(false);
    assert(Service(root,root,port).client==s.client);
    s.start_login();
    assert(s.enabled&&s.code=="CODE");
    s.publish();
    assert(read_private(root/"twitch-ui.tsv").find("device-secret")==S::npos);
    for (int i=0;i<4;++i) {
        s.device_at={};
        s.poll_login();
        assert(s.enabled&&s.code=="CODE"&&s.device_code=="device-secret");
    }
    assert(s.interval==10s&&s.device_at>Clock::now()+34s);
    s.device_at={};
    s.poll_login();
    assert(s.enabled&&s.code.empty());
    assert(Service(root,root,port).has_login());
    s.helix("test");
    assert(load(root/"oauth.json").text("refresh_token")=="new-refresh");
    s.sign_in_required("Revoked");
    assert(!fs::exists(root/"oauth.json")&&!s.enabled&&s.status=="Sign-in required");
    assert(Service(root,root,port).client=="client123456789");

    Service unavailable(root,root,port,[](const S& url,const S&,const std::vector<S>&) -> J {
        assert(url==S(OAuth)+"device");
        throw HttpError(503,"temporarily unavailable");
    });
    unavailable.request({"connect"," client123456789 "});
    unavailable.tick();
    assert(unavailable.enabled&&unavailable.requesting_code&&unavailable.status=="Reconnecting");
    unavailable.request({"disconnect"});
    assert(!unavailable.enabled&&!unavailable.requesting_code);
}

void test_event_handoff(const fs::path& root, unsigned port) {
    fs::create_directory(root);
    int opens=0, subscriptions=0;
    auto request=[&](const S& url,const S& body,const std::vector<S>&) -> J {
        if (url==S(OAuth)+"validate")
            return J::parse(R"({"client_id":"client123456789","user_id":"123","login":"sample","scopes":["channel:read:redemptions"],"expires_in":3600})");
        if (url==S(Helix)+"eventsub/subscriptions") {
            assert(J::parse(body).get("transport").text("session_id")=="initial");
            ++subscriptions;
        }
        return J::obj();
    };
    auto socket=[&](const S& url) -> std::unique_ptr<WS> {
        const S local="ws://127.0.0.1:"+std::to_string(port);
        if (url==EventWS) return std::make_unique<WS>(local+"/event-initial",true);
        assert(url=="wss://eventsub.wss.twitch.tv/ws?reconnect=mock");
        if (++opens==1) throw std::runtime_error("temporary handoff failure");
        return std::make_unique<WS>(local+"/event-replacement",true);
    };
    Service s(root,root,port,request,socket);
    s.client="client123456789";
    s.save_token(J::parse(R"({"access_token":"access","refresh_token":"refresh"})"));
    s.preference(true);
    s.connect_event();
    auto* old=s.event.get();
    s.local();
    assert(!s.api&&s.pending_api);
    auto deadline=Clock::now()+3s;
    while (!opens&&Clock::now()<deadline) {s.update_remote();std::this_thread::sleep_for(10ms);}
    assert(opens==1&&s.event.get()==old&&subscriptions==1);
    while (!s.handoff_url.empty()&&Clock::now()<deadline) {s.update_remote();s.update_local();std::this_thread::sleep_for(10ms);}
    assert(opens==2&&s.handoff_url.empty()&&s.event&&s.event.get()!=old&&subscriptions==1);
    assert(s.api&&s.model=="A");
    s.last_event=Clock::now()-s.keepalive-1s;
    s.tick();
    assert(s.status=="Reconnecting"&&s.retry>Clock::now()&&s.api);
}

int main(int argc,char** argv){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    assert(argc==3);
    auto root=fs::path(argv[1]);auto port=unsigned(std::stoul(argv[2]));
    test_device_login(root/"device",port);
    auto j=J::parse(R"({"metadata":{"message_type":"notification"},"payload":{"subscription":{"type":"channel.channel_points_custom_reward_redemption.add"},"event":{"id":"first","broadcaster_user_id":"123","reward":{"id":"reward"}}}})");
    Dedup d;assert(d.take(j,"999").empty());assert(d.take(j,"123")=="reward");assert(d.take(j,"123").empty());
    assert(verify_url("https://www.twitch.tv/activate"));assert(!verify_url("https://www.twitch.tv.evil/activate"));assert(!reconnect_url("wss://eventsub.wss.twitch.tv@evil.com"));
    assert(valid_binding(binding("SharkTail - Toggle.exp3.json","toggle","60")));assert(binding("Halo.exp3.json","toggle","60").get("duration").number()==0);
    bool rejected=false;try{binding("Halo.exp3.json","timed","301");}catch(...){rejected=true;}assert(rejected);
    rejected=false;try{binding("Halo.exp3.json","timed","nan");}catch(...){rejected=true;}assert(rejected);
    atomic(root/"kawari-token.json",R"({"token":"test-credential"})");atomic(root/"bindings.json",R"({"A":{"reward":{"expressionFile":"SharkTail - Toggle.exp3.json","mode":"toggle","duration":0},"timer":{"expressionFile":"Halo.exp3.json","mode":"timed","duration":60}}})");
    Service s(root,root,port);assert(s.current());assert(s.model=="A");assert(s.expressions.array().size()==2);s.dispatch("reward");s.dispatch("timer");
    rejected=false;try{s.dispatch("reward","other-model");}catch(...){rejected=true;}assert(rejected);
    s.publish();auto snapshot=read_private(root/"twitch-ui.tsv");assert(snapshot.find("test-credential")==S::npos);assert(snapshot.find("SharkTail - Toggle.exp3.json")!=S::npos);
    assert(load(root/"bindings.json").get("A").get("timer").get("duration").number()==60);
    // Simulate connected status with a second local WebSocket, not a Twitch account.
    s.event=std::make_unique<WS>("ws://127.0.0.1:"+std::to_string(port),true);
    s.rewards=J::parse(R"([{"id":"reward","title":"Акулий хвост","cost":500}])");
    s.publish();assert(read_private(root/"twitch-ui.tsv").find(u8"Акулий хвост")!=S::npos);
    assert(display(u8"Рожки\tЁлка")==u8"Рожки Ёлка");
    s.request({"bind","A","reward","Halo.exp3.json","timed","12"});
    assert(load(root/"bindings.json").get("A").get("reward").get("duration").number()==12);
    rejected=false;try{s.request({"bind","B","reward","Halo.exp3.json","toggle","60"});}catch(...){rejected=true;}assert(rejected);
    s.request({"test","A","reward"});s.request({"unbind","A","reward"});
    assert(!load(root/"bindings.json").get("A").get("reward").object());
    assert(load(root/"bindings.json").get("A").get("timer").get("duration").number()==60);

    s.channel="123";
    j.get("payload").get("event").get("reward").set("id","timer");
    s.api.reset();
    s.event_message(j);
    s.event_message(j);
    assert(s.redemptions.size()==1);
    s.update_local();
    assert(s.api&&s.redemptions.empty());
    s.redemptions.push_back({"previous-model","timer",Clock::now()+20s});
    s.redemptions.push_back({"A","timer",Clock::now()-1s});
    s.process_redemptions();
    s.process_redemptions();
    assert(s.redemptions.empty());
    test_event_handoff(root/"handoff",port);
}
