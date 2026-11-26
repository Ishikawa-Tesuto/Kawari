#include "studio_parameters.hpp"
#include "studio_parameter_socket.hpp"
#include <cassert>
#include <sys/stat.h>
using namespace studio::parameters;
int main(){
    Runtime r; auto t=Clock::now(); float value=0; unsigned writes=0;
    auto apply=[&](int ms,float dt=.1f){r.apply(t+std::chrono::milliseconds(ms),dt,[&](size_t){return value;},[&](size_t i,float v){assert(i==0);value=v;++writes;});};
    r.reset({{"ParamAngleX",-30,30,0}});
    assert(r.ingest("set tracking 500 ParamAngleX 90",t));apply(0);assert(value==30);
    assert(r.ingest("set plugin 100 ParamAngleX -12",t));apply(1);assert(value==-12);
    assert(r.ingest("set test 50 ParamAngleX 3",t));apply(2);assert(value==3);
    apply(51);assert(value==-12);apply(101);assert(value==30);writes=0;apply(501);assert(writes==0);
    assert(!r.ingest("set test 500 ParamAngleX 2 Missing 3",t));apply(0);assert(writes==0);
    assert(!r.ingest("set test 500 ParamAngleX nan",t));assert(!r.ingest("set test 500 ParamAngleX 1 ParamAngleX 2",t));
    std::string error;std::istringstream config("FaceX ParamAngleX -1 1 30 -30 100\n");assert(r.load_map(config,error));
    assert(r.ingest("set tracking 500 FaceX 1",t));value=0;apply(0,.1f);assert(value<-18&&value>-20);
    assert(r.ingest("clear tracking",t));writes=0;apply(1);assert(writes==0);
    std::istringstream bad("FaceX ParamAngleX -1 1 0 31 0\n");assert(!r.load_map(bad,error));assert(r.ingest("set tracking 500 FaceX -1",t));apply(3,.1f);assert(value>0);
    // VTS rows keep reversed endpoints and allow calibration outside rig bounds.
    r.reset({{"Pucker",0,2,0},{"Other",-10,10,0},{"Clamp",-10,10,0}});
    std::istringstream imported("MouthPucker Pucker 0 -0.15 0 2 0 vts 0 0\nMouthPucker Other 0 -0.15 0 3 0 vts 0 0\nMouthPucker Clamp 0 -0.15 0 3 0 vts 0 1\n");
    assert(r.load_map(imported,error));
    float vals[3]={0,0,0};
    assert(r.ingest("set plugin 500 MouthPucker -0.3",t));
    r.apply(t,.1f,[&](size_t i){return vals[i];},[&](size_t i,float v){vals[i]=v;});
    assert(vals[0]==2&&std::abs(vals[1]-6)<.0001f&&vals[2]==3);
    assert(r.ingest("set plugin 500 MouthPucker -0.075",t));
    r.apply(t,.1f,[&](size_t i){return vals[i];},[&](size_t i,float v){vals[i]=v;});
    assert(std::abs(vals[0]-1)<.0001f&&std::abs(vals[1]-1.5)<.0001f);
    std::istringstream malformed("MouthPucker Pucker 0 -0.15 0 2 0 vts 3 0\n");assert(!r.load_map(malformed,error));
    r.reset({{"Angle",-30,30,0}});
    std::istringstream stale("FaceAngleX Angle -40 40 -30 30 0 vts 0 0\nMouthPucker Missing 0 -0.15 0 2 0 vts 0 0\nOther Angle -1 1 -30 30 0 vts 0 0\n");
    assert(r.load_map(stale,error));assert(r.ingest("set plugin 500 FaceAngleX 20",t));
    value=0;apply(0);assert(value==15);assert(!r.ingest("set plugin 500 Other 1",t));
    std::istringstream strict("Other Missing 0 1 0 1 0\n");assert(!r.load_map(strict,error));
    r.reset({{"New",0,1,0}});assert(!r.ingest("set test 100 FaceX 1",t));
    // Simulate the SDK restoring the motion baseline each frame.
    r.reset({{"X",-10,10,0},{"Y",0,10,1},{"Z",0,10,2}});
    auto frame=[&](int ms){vals[0]=1;vals[1]=2;vals[2]=3;r.apply(t+std::chrono::milliseconds(ms),.1f,[&](size_t i){return vals[i];},[&](size_t i,float v){vals[i]=v;});};
    assert(r.ingest("set plugin 1000 X 2",t));
    assert(r.ingest("expr on test 100 0 X A 4 Y M 3 Z O 7",t));
    frame(50);assert(vals[0]==4&&vals[1]==4&&vals[2]==5);
    frame(100);assert(vals[0]==6&&vals[1]==6&&vals[2]==7);
    frame(150);assert(vals[0]==6&&vals[1]==6&&vals[2]==7);
    assert(!r.ingest("expr on broken 0 0 X O 3 Missing A 1",t));
    assert(r.ingest("expr off test",t+std::chrono::milliseconds(150)));
    frame(200);assert(vals[0]==4&&vals[1]==4&&vals[2]==5);
    frame(250);assert(vals[0]==2&&vals[1]==2&&vals[2]==3);
    assert(r.ingest("expr on timed 0 100 X O 8",t));frame(50);assert(vals[0]==8);frame(100);assert(vals[0]==2);
    assert(r.ingest("expr on persist 0 0 X O 8",t));frame(200);assert(vals[0]==8);
    assert(r.ingest("expr reset",t));frame(201);assert(vals[0]==2);
    // Class-based blending: overwrite, multiplication, then addition regardless of activation order.
    r.reset({{"X",-100,100,1}});
    auto mix=[&](int ms){value=2;r.apply(t+std::chrono::milliseconds(ms),.1f,[&](size_t){return value;},[&](size_t,float v){value=v;});};
    assert(r.ingest("expr on a 0 0 X A 3",t));
    assert(r.ingest("expr on m 0 0 X M 2",t));
    assert(r.ingest("expr on o 0 0 X O 4",t));mix(0);assert(value==11);
    assert(r.ingest("expr on2 newest 0 0 200 X O 6",t));mix(0);assert(value==15);
    assert(r.ingest("expr off newest",t));mix(100);assert(value==13);mix(200);assert(value==11);
    assert(r.ingest("expr on newer 0 0 X O 8",t));mix(0);assert(value==19);
    assert(r.ingest("expr on o 0 0 X O 4",t));mix(0);assert(value==11);
    assert(r.ingest("expr off o",t));mix(0);assert(value==19);
    assert(r.ingest("expr on editor-preview 0 500 X O 10",t));mix(0);assert(value==23);
    r.clear_expression_preview();mix(0);assert(value==19);
    assert(r.ingest("expr on editor-preview 0 500 X O 10",t));mix(501);assert(value==19);
    assert(r.ingest("expr reset",t));
    // Real editor timings: 3 seconds in, 0.5 seconds out, even with a fresh baseline each frame.
    assert(r.ingest("expr on2 slow 3000 0 500 X O 10",t));
    mix(0);assert(value==2);mix(750);assert(value==4);mix(1500);assert(value==6);mix(3000);assert(value==10);
    assert(r.ingest("expr off slow",t+std::chrono::milliseconds(4000)));
    mix(4250);assert(value==6);mix(4500);assert(value==2);
    // Preview auto-releases after fade-in + one second, then uses the independent fade-out.
    assert(r.ingest("expr on2 editor-preview 3000 4000 500 X O 10",t));
    mix(1500);assert(value==6);mix(3500);assert(value==10);mix(4250);assert(value==6);mix(4500);assert(value==2);
    // An explicit API off override is local to that transition.
    assert(r.ingest("expr on2 explicit 0 0 500 X O 10",t));mix(0);assert(value==10);
    assert(!r.ingest("expr off2 explicit -1",t));assert(!r.ingest("expr off2 explicit 10001",t));
    assert(r.ingest("expr off2 explicit 2000",t));mix(1000);assert(value==6);mix(2000);assert(value==2);
    // Reversing an in-progress transition starts at the current weight, not full opacity.
    assert(r.ingest("expr on2 reversal 3000 0 500 X O 10",t));mix(1500);assert(value==6);
    assert(r.ingest("expr off reversal",t+std::chrono::milliseconds(1500)));mix(1750);assert(value==4);mix(2000);assert(value==2);
    // No intermediate clamping, including when additive layers cancel.
    assert(r.ingest("expr on plus 0 0 X A 150",t));assert(r.ingest("expr on minus 0 0 X A -150",t));mix(0);assert(value==2);
    // Repeat metadata comes from the model, not a naming convention.
    const Definition cyclic("Rotation",0,16,0,true), linear("Rotation",0,16,0);
    assert(bound_value(cyclic,17)==1 && bound_value(cyclic,-1)==15);
    assert(bound_value(cyclic,49)==1 && bound_value(cyclic,-33)==15);
    assert(bound_value(cyclic,16)==16 && bound_value(cyclic,0)==0);
    assert(bound_value(cyclic,32)==0 && bound_value(cyclic,-16)==16);
    assert(bound_value(linear,17)==16 && bound_value(linear,-1)==0);
    const Definition signed_cycle("Signed",-180,180,0,true);
    assert(bound_value(signed_cycle,181)==-179 && bound_value(signed_cycle,-181)==179);
    assert(std::isfinite(bound_value(cyclic,1e100)));
    assert(smooth_value(cyclic,15,1,.5f)==16);
    assert(smooth_value(cyclic,1,15,.5f)==0);
    assert(smooth_value(linear,15,1,.5f)==8);
    r.reset({cyclic});value=0;
    assert(r.ingest("set test 500 Rotation 33",t));apply(0);assert(value==1);
    assert(r.ingest("set test 500 Rotation -1",t));apply(1);assert(value==15);
    // Imported flags still take precedence over the default unclamped identity.
    std::istringstream clip("Tracker Rotation 0 16 0 16 0 vts 1 0\n");
    assert(r.load_map(clip,error));assert(r.ingest("set test 500 Tracker 17",t));apply(2);assert(value==16);
    std::istringstream output_clip("Tracker Rotation 0 16 0 16 0 vts 0 1\n");
    assert(r.load_map(output_clip,error));apply(3);assert(value==16);
    std::istringstream unclip("Tracker Rotation 0 16 0 16 0 vts 0 0\n");
    assert(r.load_map(unclip,error));apply(4);assert(value==1);
    std::istringstream smooth("Tracker Rotation 0 16 0 16 100 vts 0 0\n");
    assert(r.load_map(smooth,error));value=15;apply(5,.1f);assert(value>0 && value<1);
    r.reset({cyclic});value=15;
    assert(r.ingest("expr on roll 0 0 Rotation A 2",t));apply(0);assert(value==1);
    assert(r.ingest("expr reset",t));value=9;
    assert(r.ingest("expr on roll 0 0 Rotation M 2",t));apply(0);assert(value==2);
    assert(r.ingest("expr reset",t));value=0;
    assert(r.ingest("expr on roll 0 0 Rotation O 35",t));apply(0);assert(value==3);
    // Editing a single target preserves other mappings and active expressions.
    r.reset({{"X",-10,10,0},{"Y",0,10,0}});
    assert(r.ingest("expr on edit 0 0 Y A 2",t));
    Mapping edit("FaceX",0,1,-1,-10,10,0);edit.clamp_input=false;
    assert(r.replace_mapping(edit,error));
    assert(r.ingest("set plugin 500 FaceX -0.5",t));
    float raw=0;int source=-1;
    assert(r.live_input("FaceX",t,raw,source)&&raw==-.5f&&source==1);
    assert(!r.live_input("FaceX",t+std::chrono::milliseconds(500),raw,source));
    vals[0]=0;vals[1]=0;
    r.apply(t,.1f,[&](size_t i){return vals[i];},[&](size_t i,float v){vals[i]=v;});
    assert(vals[0]==5&&vals[1]==2);
    const auto serialized=r.serialize_mappings();
    Runtime reopened;reopened.reset({{"X",-10,10,0},{"Y",0,10,0}});
    std::istringstream saved(serialized);assert(reopened.load_map(saved,error));
    assert(reopened.serialize_mappings()==serialized);
    edit.in_max=edit.in_min;assert(!r.replace_mapping(edit,error));
    assert(r.serialize_mappings()==serialized);
    edit.in_max=-1;edit.input="Bad#ID";assert(!r.replace_mapping(edit,error));
    assert(r.serialize_mappings()==serialized);
    r.reset({{"New",0,1,0}});
    Socket receiver;assert(receiver.open_socket());auto path=receiver.path();struct stat st{};assert(::stat(path.substr(0,path.rfind('/')).c_str(),&st)==0);assert((st.st_mode&0777)==0700);
    int fd=::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_un addr{};addr.sun_family=AF_UNIX;std::memcpy(addr.sun_path,path.c_str(),path.size()+1);
    std::string msg="set test 100 New 1";assert(::sendto(fd,msg.data(),msg.size(),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==static_cast<ssize_t>(msg.size()));
    int got=0;receiver.drain([&](const std::string& p){assert(p==msg);assert(r.ingest(p,t));++got;});assert(got==1);::close(fd);receiver.close_socket();assert(::access(path.c_str(),F_OK)!=0);
}
