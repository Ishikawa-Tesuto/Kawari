#define main kawari_twitch_main
#include "../native/twitch_bridge.cpp"
#undef main
#include <cassert>
int main(int argc,char** argv){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    assert(argc==3);
    auto root=fs::path(argv[1]);auto port=unsigned(std::stoul(argv[2]));
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
}
