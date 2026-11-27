#include "studio_controls.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <glob.h>
#include <unistd.h>
int test_key=-1;
void press(GLFWwindow& w,int key) {test_key=key;studio::update_view(&w);test_key=-1;studio::update_view(&w);}
void test_model_views(const std::string& dir,bool restore) {
    const std::string first=dir+"/first.tsv",second=dir+"/second.tsv";
    setenv("STUDIO_VIEW_PROFILE",first.c_str(),1);
    setenv("STUDIO_HOT_SWITCH","1",1);setenv("STUDIO_LIBRARY_DIR",dir.c_str(),1);
    GLFWwindow window;
    studio::update_view(&window);
    auto check=[](float x,float y,float scale){
        float transform[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        studio::apply_camera(transform);
        assert(std::abs(transform[12]-x)<.0001f && std::abs(transform[13]-y)<.0001f);
        assert(std::abs(transform[0]-scale)<.0001f && std::abs(studio::camera_scale()-scale)<.0001f);
    };
    auto next_frame=[&](){
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        studio::update_view(&window);
    };
    auto switch_to=[&](const std::string& profile){
        const std::string mailbox=dir+"/load.txt";
        {std::ofstream request(mailbox);request<<"/model.model3.json\n/model.model3.json\n\n0\n"<<profile<<'\n';}
        studio::ModelSelection selection;
        assert(studio::take_model_selection(selection));
        studio::commit_model_selection(selection);
        studio::update_view(&window);
    };
    if(!restore){
        check(0,0,1);
        studio::camera_pan(.5f,.25f);
        studio::camera_scroll(1.f,0,0);
        assert(studio::expression_preview("expr on obs-position 0 0 position V 0.7 0.25 20 1.2"));
        next_frame();
        check(.7f,.25f,1.2f);
        assert(studio::expression_preview("expr hold obs-position"));
        next_frame();
        check(.7f,.25f,1.2f);

        assert(studio::expression_preview("expr on obs-position 0 0 position V -1 -1 1 2"));
        next_frame();
        float x,y;studio::camera_offset(x,y);
        const float scale=studio::camera_scale();
        assert(x<.7f&&x>-1.f&&scale>1.2f&&scale<2.f);
        assert(studio::expression_preview("expr hold obs-position"));
        next_frame();
        check(x,y,scale);

        switch_to(second);
        check(0,0,1);
        assert(studio::expression_preview("expr on pose 0 0 position V 0.2 0.1 20 1.25"));
        next_frame();
        check(.2f,.1f,1.25f);
        studio::camera_pan(-.2f,.3f);
        check(0,.4f,1.25f);
        next_frame();
        check(0,.4f,1.25f);
        studio::camera_zoom(1.6f);
        check(0,.4f,2.f);
        assert(studio::expression_preview("expr off pose"));
        next_frame();
        check(0,.4f,2.f);

        assert(studio::expression_preview("expr on pose 0 0 position V 0.1 0.4 20 2"));
        assert(studio::expression_preview("expr off pose"));
        next_frame();
        check(.1f,.4f,2.f);
        next_frame();
        check(.1f,.4f,2.f);

        assert(studio::expression_preview("expr on editor-preview 0 500 position V 0.3 0.4 20 2"));
        next_frame();
        check(.3f,.4f,2.f);
        studio::expression_preview_clear();
        next_frame();
        check(.1f,.4f,2.f);
        switch_to(first);
    }
    check(.5375f,.26875f,1.075f);
    switch_to(second);
    check(0,.4f,2.f);
    if(restore){studio::camera_reset();studio::camera_save();check(0,0,1);}
}
int main(int argc,char** argv) {
    if(argc==3){test_model_views(argv[1],std::string(argv[2])=="restore");return 0;}
    GLFWwindow w;
    assert(studio::camera_is_default());
    assert(!studio::demo_visible());
    press(w,GLFW_KEY_F2); assert(studio::demo_visible());
    press(w,GLFW_KEY_B); assert(!studio::demo_visible()); assert(studio::background()>.1f);
    press(w,GLFW_KEY_EQUAL); press(w,GLFW_KEY_D);
    float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    studio::apply_camera(m); assert(std::abs(m[0]-1.12f)<.001f); assert(std::abs(m[12]-.08f)<.001f);
    press(w,GLFW_KEY_HOME); assert(studio::camera_is_default());
    studio::camera_zoom(100.f);assert(studio::camera_scale()==5.f);
    studio::camera_zoom(.001f);assert(studio::camera_scale()==.2f);
    studio::camera_pan(100.f,-100.f);
    float p[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    studio::apply_camera(p);assert(p[12]==100.f && p[13]==-100.f);
    studio::camera_reset();assert(studio::camera_is_default());
    studio::camera_zoom_at(2.f,.5f,-.25f);
    float z[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    studio::apply_camera(z);assert(z[0]==2.f && z[12]==-.5f && z[13]==.25f);
    assert(z[0]*.5f+z[12]==.5f && z[5]*(-.25f)+z[13]==-.25f);
    studio::camera_zoom_at(.5f,.5f,-.25f);assert(studio::camera_is_default());
    // At 500% the former +/-3 clip-space clamp prevented moving to the edges.
    studio::camera_zoom(5);studio::camera_pan(8,-8);
    float wide[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    studio::apply_camera(wide);assert(wide[12]==8 && wide[13]==-8);
    studio::camera_reset();studio::camera_scroll(1,.5f,-.25f);
    assert(studio::camera_scale()==1);studio::camera_advance(1.f/60);
    assert(studio::camera_scale()>1&&studio::camera_scale()<1.075f);
    for(int i=0;i<90;++i){studio::camera_advance(1.f/60);float q[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};studio::apply_camera(q);assert(std::abs(q[0]*.5f+q[12]-.5f)<1e-5f);assert(std::abs(q[5]*(-.25f)+q[13]+.25f)<1e-5f);}
    assert(std::abs(studio::camera_scale()-1.075f)<1e-6f);
    studio::camera_reset();studio::camera_scroll(.5f,.2f,.1f);studio::camera_scroll(.5f,.2f,.1f);
    studio::camera_advance(.016f);studio::camera_pan(.3f,-.4f);
    for(int i=0;i<90;++i)studio::camera_advance(1.f/60);
    float dragged[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};studio::apply_camera(dragged);
    assert(std::abs(dragged[0]*.2f+dragged[12]-.5f)<1e-5f);
    assert(std::abs(dragged[5]*.1f+dragged[13]+.3f)<1e-5f);
    assert(std::abs(studio::camera_scale()-1.075f)<1e-6f);
    studio::camera_scroll(100,0,0);for(int i=0;i<120;++i)studio::camera_advance(1.f/60);assert(studio::camera_scale()==5);
    studio::camera_scroll(-100,0,0);for(int i=0;i<120;++i)studio::camera_advance(1.f/60);assert(studio::camera_scale()==.2f);
    studio::camera_scroll(8,0,0);studio::camera_reset();studio::camera_advance(.1f);assert(studio::camera_is_default());
    // Equal elapsed time produces the same zoom at different render rates.
    studio::camera_scroll(8,0,0);for(int i=0;i<15;++i)studio::camera_advance(1.f/30);float low=studio::camera_scale();
    studio::camera_reset();studio::camera_scroll(8,0,0);for(int i=0;i<60;++i)studio::camera_advance(1.f/120);assert(std::abs(low-studio::camera_scale())<.0003f);
    studio::camera_reset();
    const float old=studio::background();studio::cycle_background();assert(studio::background()!=old);
    setenv("STUDIO_UI","1",1);setenv("STUDIO_BROADCAST","1",1);
    press(w,GLFW_KEY_F2);assert(!studio::demo_visible());
    const float ui_bg=studio::background();
    press(w,GLFW_KEY_B);assert(studio::background()==ui_bg);
    press(w,GLFW_KEY_D);assert(studio::camera_is_default());
    press(w,GLFW_KEY_ESCAPE);assert(!w.closed);
    unsetenv("STUDIO_UI");unsetenv("STUDIO_BROADCAST");
    setenv("STUDIO_WIDTH","1920",1);assert(studio::window_width(800)==1920);
    setenv("STUDIO_WIDTH","-1",1);assert(studio::window_width(800)==800);
    char dir[]="/tmp/studio-snapshot-XXXXXX"; assert(mkdtemp(dir));
    setenv("XDG_RUNTIME_DIR",dir,1); setenv("STUDIO_PARAMETER_INPUT","1",1);
    Live2D::Cubism::Framework::CubismModel model;
    test_key=GLFW_KEY_RIGHT;studio::update_controls(&model,&w);assert(std::abs(model.value-.1f)<.001f);
    test_key=-1;studio::update_controls(&model,&w);
    test_key=GLFW_KEY_R;studio::update_controls(&model,&w);model.value=0;test_key=-1;studio::update_controls(&model,&w);assert(model.value==0);
    studio::apply_manual_overrides(&model);
    glob_t files{}; const std::string pattern=std::string(dir)+"/vtuber-params-*/parameters.tsv";
    assert(glob(pattern.c_str(),0,nullptr,&files)==0); assert(files.gl_pathc==1);
    std::ifstream snapshot(files.gl_pathv[0]); std::string line; std::getline(snapshot,line);
    assert(line.find("P\tParamTest\t")==0); std::getline(snapshot,line);
    assert(line.find("M\tParamTest\tParamTest\t")==0);
    globfree(&files);
    studio::reset_controls();model.repeat=true;model.value=.95f;
    test_key=GLFW_KEY_RIGHT;studio::update_controls(&model,&w);
    assert(std::abs(model.value-(-.95f))<.001f);
    studio::reset_controls();model.repeat=false;model.value=.95f;
    studio::update_controls(&model,&w);assert(model.value==1.f);
    test_key=-1;
    // Live edits, explicit save, reload and release of a temporary test override.
    const std::string map=std::string(dir)+"/parameters.map";
    {std::ofstream out(map);out<<"# profile\n";}
    setenv("STUDIO_PARAMETER_MAP",map.c_str(),1);setenv("STUDIO_EDITABLE_MAP",map.c_str(),1);
    setenv("STUDIO_UI","1",1);setenv("STUDIO_BROADCAST","1",1);
    studio::reset_controls();model.value=0;studio::update_controls(&model,&w);
    assert(studio::editor_can_save());
    test_key=GLFW_KEY_RIGHT;studio::update_controls(&model,&w);assert(model.value==0);
    test_key=-1;studio::update_controls(&model,&w);
    studio::parameters::Mapping binding("FaceX",0,-2,2,-1,1,12);
    std::string error;assert(studio::editor_apply_mapping(binding,error));assert(studio::editor_dirty());
    {std::ifstream old(map);std::getline(old,line);assert(line=="# profile");}
    studio::editor_preview(0,.5f);studio::apply_manual_overrides(&model);
    assert(model.value==.5f&&studio::editor_values()[0]==.5f);
    studio::editor_release();model.value=-.25f;studio::apply_manual_overrides(&model);assert(model.value==-.25f);
    assert(studio::editor_save(error));assert(!studio::editor_dirty());
    studio::reset_controls();studio::update_controls(&model,&w);
    assert(studio::editor_runtime().mappings()[0].input=="FaceX");
    assert(studio::editor_runtime().mappings()[0].smoothing_ms==12);
    unsetenv("STUDIO_EDITABLE_MAP");assert(!studio::editor_save(error));
    unsetenv("STUDIO_PARAMETER_MAP");unsetenv("STUDIO_UI");unsetenv("STUDIO_BROADCAST");
    // Prepared mailbox is consumed once; switching rotates IPC, drops old
    // manual values and loads the new mapping rather than the startup env.
    setenv("STUDIO_HOT_SWITCH","1",1);setenv("STUDIO_LIBRARY_DIR",dir,1);
    const std::string load=std::string(dir)+"/load.txt";
    studio::ModelSelection selection;
    {std::ofstream f(load);f<<"relative\n/Model.model3.json\n\n1\n";}
    assert(!studio::take_model_selection(selection));
    assert(access(load.c_str(),F_OK)!=0);
    {std::ofstream f(load);f<<"/Model A.model3.json\n/Model A.model3.json\n"<<map<<"\n1\n";}
    assert(studio::take_model_selection(selection));assert(!studio::take_model_selection(selection));
    studio::editor_preview(0,.75f);studio::camera_zoom(2.f);
    assert(glob(pattern.c_str(),0,nullptr,&files)==0);std::string previous=files.gl_pathv[0];globfree(&files);
    studio::commit_model_selection(selection);assert(studio::camera_is_default());assert(studio::active_original_model()=="/Model A.model3.json");
    assert(access(previous.c_str(),F_OK)!=0);assert(studio::editor_values().empty());
    model.value=0;studio::update_controls(&model,&w);studio::apply_manual_overrides(&model);
    assert(studio::editor_runtime().mappings()[0].input=="FaceX" && studio::editor_can_save());assert(model.value==0);
    assert(glob(pattern.c_str(),0,nullptr,&files)==0);assert(files.gl_pathc==1);assert(previous!=files.gl_pathv[0]);globfree(&files);
    unsetenv("STUDIO_HOT_SWITCH");unsetenv("STUDIO_LIBRARY_DIR");
    unlink(map.c_str());
    press(w,GLFW_KEY_ESCAPE); assert(w.closed);
}
