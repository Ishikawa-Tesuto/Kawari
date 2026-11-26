#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "studio_app_icon.hpp"
#include "studio_ui.hpp"
#include "studio_ui_layout.hpp"
#include "studio_controls.hpp"
#include "studio_physics.hpp"
#include "studio_animations.hpp"
#include "studio_items.hpp"
#include "studio_render_size.hpp"
#include "imgui.h"
#include "studio_ui_theme.hpp"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <fstream>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <locale>
#include <map>
#include <iterator>
#include <chrono>
#include <sys/stat.h>
#include <time.h>


namespace studio {
namespace {
bool initialized=false;
bool frame_ready=false;
bool dragging=false;
bool toggle_fill=false,toggle_compact=false;
bool ui_prefs_loaded=false;
int applied_ui_scale=100;
bool app_icon_ready=false;
ImVec2 app_icon_uv_min,app_icon_uv_max;
std::string ui_prefs_saved,ui_prefs_candidate;
std::chrono::steady_clock::time_point ui_prefs_changed;
void restore_workspace_preferences();
void persist_workspace_preferences(bool force=false);
ImVec2 previous_mouse;
VkDevice device=VK_NULL_HANDLE;
VkFormat format=VK_FORMAT_UNDEFINED;
unsigned image_count=0;
void vk_result(VkResult r) {
    if(r != VK_SUCCESS) std::fprintf(stderr,"[ui] Vulkan result: %d\n",int(r));
}
std::string model_name() {
    std::string s=active_runtime_model();if(s.empty())s="Live2D model";
    auto slash=s.find_last_of('/'); if(slash!=std::string::npos) s.erase(0,slash+1);
    const std::string suffix=".model3.json";
    if(s.size()>=suffix.size() && s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0) s.resize(s.size()-suffix.size());
    return s;
}
std::vector<std::string> library_models;
char model_path[8192]{},library_filter[129]{};
bool library_show_paths=false;
bool switch_pending=false;
bool discard_on_switch=false;
bool library_open=false,library_requested=false;
std::string library_error;
std::string library_dir() {
    const char* p=std::getenv("STUDIO_LIBRARY_DIR"); return p?p:"";
}
void read_library(const std::string& dir) {
    library_models.clear();
    std::ifstream f(dir+"/models.txt");std::string line;
    while(std::getline(f,line)) if(!line.empty()) library_models.push_back(line);
}
bool parameter_draft_pending();
bool physics_open=false,physics_focus=false;
bool animations_open=false,animations_focus=false;
char animations_filter[129]{};
int animation_selected=-1;
bool animation_dirty(){auto* e=animation::active();return e&&e->dirty();}
char physics_filter[129]{};
bool physics_groups_open=false,physics_adjusted_only=false;
bool physics_dirty(){auto* e=physics::active();return e&&e->dirty();}
bool request_model(const std::string& dir,const std::string& path) {
    if(path.empty()||path.front()!='/'||path.find_first_of("\r\n\t")!=std::string::npos) {
        library_error="Enter an absolute .model3.json path."; return false;
    }
    if((editor_dirty()||parameter_draft_pending()||physics_dirty()||animation_dirty())&&!discard_on_switch){library_error="Save model settings or explicitly discard them before switching.";return false;}
    const auto tmp=dir+"/select.tmp";
    std::remove((dir+"/loaded.txt").c_str());
    std::remove((dir+"/error.txt").c_str());
    {std::ofstream f(tmp,std::ios::binary|std::ios::trunc); f<<path; f.close();
     if(!f) {library_error="Could not write selection request.";return false;}}
    if(std::rename(tmp.c_str(),(dir+"/select.txt").c_str())!=0) {
        std::remove(tmp.c_str()); library_error="Could not submit selection request.";return false;
    }
    library_error.clear(); return true;
}
void library_popup(const ImVec2& sz) {
    const auto dir=library_dir(); if(dir.empty()) return;
    if(switch_pending) {
        std::ifstream done(dir+"/loaded.txt");std::string ready;if(std::getline(done,ready)&&ready==active_original_model()){switch_pending=false;read_library(dir);library_error.clear();}
        std::ifstream f(dir+"/error.txt"); if(f) {
            std::string error; std::getline(f,error);if(!error.empty()) {library_error=error;switch_pending=false;}
        }
    }
    if(library_requested){library_requested=false;read_library(dir);discard_on_switch=false;library_open=true;
        if(!model_path[0]&&active_original_model().size()<sizeof(model_path))std::snprintf(model_path,sizeof(model_path),"%s",active_original_model().c_str());
        ImGui::OpenPopup(ui::tr("Model Library###model_library","Библиотека моделей###model_library"));dragging=false;}
    if(!library_open)return;
    if(!ImGui::IsPopupOpen(ui::tr("Model Library###model_library","Библиотека моделей###model_library"))){library_open=false;return;}
    ui::place_tool(sz,float(ui_bar_height(int(sz.y))),650.f,440.f,true);
    if(!ImGui::BeginPopupModal(ui::tr("Model Library###model_library","Библиотека моделей###model_library"),&library_open,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings))return;
    const auto& current=active_original_model();
    ImGui::TextUnformatted("Library");ImGui::SameLine();ImGui::TextDisabled("%zu models",library_models.size());
    ImGui::BeginDisabled(switch_pending);
    if(ImGui::SmallButton("Refresh"))read_library(dir);
    ImGui::SameLine();ImGui::Checkbox("Show paths",&library_show_paths);
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##library_filter","Search model name or path",library_filter,sizeof(library_filter));
    const bool dirty=editor_dirty()||parameter_draft_pending()||physics_dirty()||animation_dirty();
    ImGui::BeginChild("##models",ImVec2(0,std::max(45.f,ImGui::GetContentRegionAvail().y-(dirty?155.f:125.f)*ui::scale())),ImGuiChildFlags_Borders);
    size_t matches=0;
    for(size_t i=0;i<library_models.size();++i) {
        const auto& path=library_models[i];const bool active=path==current;
        const auto name=ui_model_label(path);
        if(!ui_parameter_matches(path,library_filter)&&!ui_parameter_matches(name,library_filter))continue;
        ++matches;
        ImGui::PushID(path.c_str());
        // A stable ID is independent of the model name or the loaded badge.
        if(ImGui::Selectable((name+(active?"  [Active]":"")+"###model").c_str(),path==model_path,ImGuiSelectableFlags_NoAutoClosePopups,ImVec2(0,32)) && path.size()<sizeof(model_path))
            std::snprintf(model_path,sizeof(model_path),"%s",path.c_str());
        if(ImGui::IsItemHovered()){ImGui::BeginTooltip();ImGui::TextUnformatted(path.c_str());ImGui::EndTooltip();}
        if(library_show_paths)ImGui::TextWrapped("%s",path.c_str());
        ImGui::PopID();
    }
    if(!matches)ImGui::TextDisabled(library_models.empty()?"No models in library.":"No matching models.");
    ImGui::EndChild();
    ImGui::TextUnformatted("Model file");ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##modelpath","/path/to/model.model3.json",model_path,sizeof(model_path));
    if(!hot_switch_enabled())ImGui::TextWrapped("Loading restarts the renderer and API.");
    if(dirty)ImGui::Checkbox("Discard unsaved model settings",&discard_on_switch);
    ImGui::BeginDisabled(!model_path[0]||current==model_path);
    if(ImGui::Button("Load model"))switch_pending=request_model(dir,model_path);
    ImGui::EndDisabled();
    ImGui::EndDisabled(); ImGui::SameLine();
    if(ImGui::Button("Close") || (ImGui::IsKeyPressed(ImGuiKey_Escape,false)&&!ImGui::IsAnyItemActive()&&!ImGui::GetIO().WantTextInput)){library_open=false;ImGui::CloseCurrentPopup();}
    if(switch_pending) ImGui::TextDisabled("Preparing model...");
    else if(!library_error.empty()) ImGui::TextWrapped("%s",library_error.c_str());
    ImGui::EndPopup();
}
// Floating editor: keep the model visible while calibrating.
int parameter_selected=-1;
char parameter_filter[129]{},input_id[129]{},input_filter[129]{},catalog_filter[129]{};
std::vector<UiInput> input_catalog;
std::string input_catalog_dir;
std::chrono::steady_clock::time_point input_catalog_tick;
bool input_catalog_ready=false,show_inputs=false;
int catalog_kind=0;
bool input_kind_matches(const UiInput& p){return catalog_kind==0||(catalog_kind==1&&p.kind=="plugin")||(catalog_kind==2&&p.kind=="builtin")||(catalog_kind==3&&p.kind=="mapping");}
void refresh_input_catalog(){
    const auto dir=expression_ui_directory();const auto now=std::chrono::steady_clock::now();
    if(dir!=input_catalog_dir){input_catalog.clear();input_catalog_ready=false;input_catalog_dir=dir;input_catalog_tick={};}
    if(dir.empty()||now-input_catalog_tick<std::chrono::milliseconds(200))return;
    input_catalog_tick=now;std::ifstream f(dir+"/input-ui.tsv");
    input_catalog_ready=f&&ui_parse_inputs(f,input_catalog);if(!input_catalog_ready)input_catalog.clear();
}
const UiInput* find_ui_input(const std::string& name){
    for(const auto& p:input_catalog)if(p.name==name)return &p;
    return nullptr;
}
void range_meter(float start,float end,float value,bool live){
    const auto p=ImGui::GetCursorScreenPos();const float w=std::max(20.f,ImGui::GetContentRegionAvail().x);
    const float h=12.f;auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(p,ImVec2(p.x+w,p.y+h),ImGui::GetColorU32(ImGuiCol_FrameBg),0.f);
    if(live&&start!=end&&std::isfinite(value)&&std::isfinite(start)&&std::isfinite(end)){
        const float x=p.x+5.f+(w-10.f)*ui_range_position(value,start,end);
        draw->AddCircleFilled(ImVec2(x,p.y+h*.5f),4.f,IM_COL32(235,100,112,255));
    }
    ImGui::Dummy(ImVec2(w,h));
}

parameters::Mapping draft("",0,0,1,0,1,0);
bool draft_changed=false, preview_active=false, parameter_open=false, parameter_focus=false;
float preview_value=0;
bool parameter_draft_pending(){return draft_changed;}
std::string parameter_status;
void stop_preview(){editor_release();preview_active=false;}
void select_parameter(int index){
    stop_preview();parameter_selected=index;draft_changed=false;parameter_status.clear();
    const auto& rt=editor_runtime();
    if(index<0||size_t(index)>=rt.definitions().size())return;
    const auto& d=rt.definitions()[index];
    draft=parameters::Mapping(d.id,index,d.minimum,d.maximum,d.minimum,d.maximum,0);
    draft.clamp_input=!d.repeat;
    for(const auto& m:rt.mappings())if(m.target==size_t(index)){draft=m;break;}
    std::snprintf(input_id,sizeof(input_id),"%s",draft.input.c_str());
    const auto& v=editor_values();preview_value=size_t(index)<v.size()?v[index]:d.initial;
}
void parameter_popup(const ImVec2& sz){
    parameter_focus=false;
    if(!parameter_open){if(preview_active)stop_preview();return;}
    if(!ui::begin_tool("Parameters###Parameter Editor",parameter_open,sz,float(ui_bar_height(int(sz.y))),720.f,650.f)){
        if(preview_active)stop_preview();
        ImGui::End();return;
    }
    parameter_focus=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const auto& rt=editor_runtime();const auto& defs=rt.definitions();const auto& values=editor_values();
    if(parameter_selected>=int(defs.size())||(parameter_selected<0&&!defs.empty()))select_parameter(defs.empty()?-1:0);
    refresh_input_catalog();
    if(ui::tool_button("Mappings",!show_inputs))show_inputs=false;
    ImGui::SameLine();if(ui::tool_button("Tracking inputs",show_inputs))show_inputs=true;
    ImGui::SameLine();ImGui::TextDisabled("%zu outputs / %zu inputs",defs.size(),input_catalog.size());
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##filter",show_inputs?"Search input or plugin":"Search input / output",show_inputs?catalog_filter:parameter_filter,sizeof(parameter_filter));
    if(show_inputs){
        if(preview_active)stop_preview();
        ImGui::SetNextItemWidth(-1);ImGui::Combo("##catalog_kind",&catalog_kind,"All inputs\0Plugins\0Default API inputs\0Model mapping inputs\0");
        ImGui::BeginChild("##input_catalog",ImVec2(0,std::max(40.f,ImGui::GetContentRegionAvail().y-45.f*ui::scale())),ImGuiChildFlags_Borders);
        if(!input_catalog_ready)ImGui::TextDisabled("Waiting for API input catalog...");
        for(const auto& p:input_catalog){
            if(!input_kind_matches(p)||(!ui_parameter_matches(p.name,catalog_filter)&&!ui_parameter_matches(p.owner,catalog_filter)))continue;
            ImGui::PushID(p.name.c_str());ImGui::TextUnformatted(p.name.c_str());
            ImGui::SameLine();ImGui::TextDisabled("%s%s",p.kind=="mapping"?"Model mapping":p.owner.c_str(),p.routed?" / mapped":" / unmapped");
            if(p.live)ImGui::Text("%.4f",p.value);else ImGui::TextDisabled("No live sample");
            range_meter(p.minimum,p.maximum,p.value,p.live);
            ImGui::TextDisabled("%.3f .. %.3f (default %.3f)",p.minimum,p.maximum,p.initial);
            ImGui::Separator();ImGui::PopID();
        }
        ImGui::EndChild();if(ImGui::Button("Back to mappings"))show_inputs=false;ImGui::SameLine();if(ImGui::Button("Close")){stop_preview();parameter_open=false;parameter_focus=false;}ImGui::End();return;
    }
    const float height=std::max(40.f,ImGui::GetContentRegionAvail().y-112.f*ui::scale());
    const float left=std::max(70.f,ImGui::GetContentRegionAvail().x*.34f);
    ImGui::BeginChild("##parameter_list",ImVec2(left,height),ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(draft_changed);
    for(size_t i=0;i<defs.size();++i){
        const auto& d=defs[i];
        const parameters::Mapping* mapping=nullptr;
        for(const auto& m:rt.mappings())if(m.target==i){mapping=&m;break;}
        if(!ui_parameter_matches(d.id,parameter_filter)&&(!mapping||!ui_parameter_matches(mapping->input,parameter_filter)))continue;
        ImGui::PushID(int(i));
        if(ImGui::Selectable((d.id+"##param").c_str(),parameter_selected==int(i),ImGuiSelectableFlags_NoAutoClosePopups))select_parameter(int(i));
        if(mapping)ImGui::TextDisabled("IN: %s",mapping->input.c_str());
        if(i<values.size())ImGui::TextDisabled("OUT: %.3f%s",values[i],d.repeat?"  Repeat":"");
        ImGui::PopID();
    }
    ImGui::EndDisabled();ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("##parameter_details",ImVec2(0,height),ImGuiChildFlags_Borders);
    if(parameter_selected>=0&&size_t(parameter_selected)<defs.size()){
        const auto& d=defs[parameter_selected];
        const float actual=size_t(parameter_selected)<values.size()?values[parameter_selected]:d.initial;
        auto edit_float=[&](const char* label,float& v){ImGui::SetNextItemWidth(std::max(60.f,ImGui::GetContentRegionAvail().x*.55f));if(ImGui::InputFloat(label,&v,0,0,"%.4f")){draft_changed=true;parameter_status.clear();}};
        ImGui::TextUnformatted("INPUT");
        ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##known_inputs",draft.input.empty()?"Select tracking input":draft.input.c_str())){
            ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##input_filter","Search input / plugin",input_filter,sizeof(input_filter));
            if(!input_catalog_ready)ImGui::TextDisabled("API catalog unavailable; routed inputs only");
            for(const auto& p:input_catalog){
                if(!ui_parameter_matches(p.name,input_filter)&&!ui_parameter_matches(p.owner,input_filter))continue;
                ImGui::PushID(p.name.c_str());
                const auto label=p.name+"  ["+p.owner+"]";
                if(ImGui::Selectable(label.c_str(),draft.input==p.name)){
                    draft.input=p.name;std::snprintf(input_id,sizeof(input_id),"%s",p.name.c_str());
                    draft_changed=true;parameter_status.clear();
                }
                ImGui::PopID();
            }
            std::vector<std::string> inputs;for(const auto& m:rt.mappings())inputs.push_back(m.input);
            std::sort(inputs.begin(),inputs.end());inputs.erase(std::unique(inputs.begin(),inputs.end()),inputs.end());
            for(const auto& n:inputs)if(!find_ui_input(n)&&ui_parameter_matches(n,input_filter)&&ImGui::Selectable(n.c_str(),draft.input==n)){
                draft.input=n;std::snprintf(input_id,sizeof(input_id),"%s",n.c_str());draft_changed=true;parameter_status.clear();
            }
            ImGui::EndCombo();
        }
        if(ImGui::TreeNode("Manual input ID")){
            ImGui::SetNextItemWidth(-1);
            if(ImGui::InputTextWithHint("##input","Input parameter ID",input_id,sizeof(input_id))){draft.input=input_id;draft_changed=true;parameter_status.clear();}
            ImGui::TreePop();
        }
        const auto* selected_input=find_ui_input(draft.input);
        float raw=0;int src=-1;
        bool live=rt.live_input(draft.input,parameters::Clock::now(),raw,src);
        if(!live&&selected_input&&selected_input->live){raw=selected_input->value;live=true;}
        if(selected_input){
            ImGui::TextDisabled("%s / %.3f .. %.3f",selected_input->owner.c_str(),selected_input->minimum,selected_input->maximum);
            if(ImGui::Button("Use input range")){draft.in_min=selected_input->minimum;draft.in_max=selected_input->maximum;draft_changed=true;}
        }else ImGui::TextDisabled("Input not registered in API catalog");
        if(live)ImGui::Text("Current: %.4f",raw);else ImGui::TextDisabled("No live sample");
        range_meter(draft.in_min,draft.in_max,raw,live);
        edit_float("IN start",draft.in_min);edit_float("IN end",draft.in_max);
        if(ImGui::Checkbox("Limit input range",&draft.clamp_input))draft_changed=true;
        ImGui::Separator();
        ImGui::TextUnformatted("OUTPUT");ImGui::SameLine();ImGui::TextDisabled("%s%s",d.id.c_str(),d.repeat?" / Repeat":"");
        ImGui::Text("Current: %.4f",actual);
        range_meter(draft.out_min,draft.out_max,actual,true);
        ImGui::TextDisabled("Model range: %.3f .. %.3f",d.minimum,d.maximum);
        edit_float("OUT start",draft.out_min);edit_float("OUT end",draft.out_max);
        if(ImGui::Checkbox("Limit output range",&draft.clamp_output))draft_changed=true;
        if(ImGui::Button("Invert output")){std::swap(draft.out_min,draft.out_max);draft_changed=true;}
        ImGui::SameLine();if(ImGui::Button("Use model range")){draft.out_min=d.minimum;draft.out_max=d.maximum;draft_changed=true;}
        ImGui::Separator();
        edit_float("Smoothing (ms)",draft.smoothing_ms);
        if(ImGui::Button("Identity")){
            draft=parameters::Mapping(d.id,size_t(parameter_selected),d.minimum,d.maximum,d.minimum,d.maximum,0);
            draft.clamp_input=!d.repeat;std::snprintf(input_id,sizeof(input_id),"%s",d.id.c_str());draft_changed=true;parameter_status.clear();
        }
        if(ImGui::TreeNode("Test output")){
            ImGui::BeginDisabled(!(d.maximum>d.minimum));
            if(!preview_active)preview_value=actual;
            ImGui::SetNextItemWidth(-1);
            if(ImGui::SliderFloat("##preview",&preview_value,d.minimum,d.maximum,"Test %.3f",ImGuiSliderFlags_AlwaysClamp)){
                editor_preview(size_t(parameter_selected),preview_value);preview_active=true;
            }
            if(ImGui::Button("Test default")){preview_value=d.initial;editor_preview(size_t(parameter_selected),preview_value);preview_active=true;}
            ImGui::SameLine();if(ImGui::Button("Release test"))stop_preview();
            ImGui::EndDisabled();ImGui::TreePop();
        }else if(preview_active)stop_preview();
        if(std::isfinite(draft.in_min)&&std::isfinite(draft.in_max)&&draft.in_min!=draft.in_max&&std::isfinite(draft.in_max-draft.in_min)&&std::isfinite(draft.out_min)&&std::isfinite(draft.out_max)){
            float curve[65];const float lo=std::min(draft.in_min,draft.in_max),hi=std::max(draft.in_min,draft.in_max);
            for(int i=0;i<65;++i)curve[i]=parameters::mapped_value(d,draft,lo+(hi-lo)*(i/64.f));
            ImGui::PlotLines("##curve",curve,65,0,"Input -> target (before smoothing)",d.minimum,d.maximum>d.minimum?d.maximum:d.minimum+1,ImVec2(-1,65));
        }
    }else ImGui::TextDisabled("Waiting for model parameters...");
    ImGui::EndChild();
    ImGui::BeginDisabled(!draft_changed);
    if(ImGui::Button("Apply live")){
        std::string error;if(editor_apply_mapping(draft,error)){draft_changed=false;stop_preview();parameter_status="Applied. Save to keep after restart.";}
        else parameter_status=error;
    }
    ImGui::SameLine();if(ImGui::Button("Discard draft"))select_parameter(parameter_selected);
    ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(draft_changed||!editor_dirty()||!editor_can_save());
    if(ImGui::Button("Save profile")){std::string error;parameter_status=editor_save(error)?"Saved to profile.":error;}
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("Close")){stop_preview();parameter_open=false;parameter_focus=false;}
    if(draft_changed)ImGui::TextDisabled("Unapplied draft: apply or discard before switching models.");
    else if(!editor_can_save())ImGui::TextDisabled("Session only: no writable profile map.");
    else if(editor_dirty())ImGui::TextDisabled("Applied changes are not saved.");
    if(!parameter_status.empty())ImGui::TextWrapped("%s",parameter_status.c_str());
    ImGui::End();
}
struct ExpressionRow {std::string key,name;bool active=false,hotkey=false;unsigned chord=0;bool global=false;};
std::vector<ExpressionRow> expression_rows;
std::string expression_dir,expression_status,recording_key,global_status;
char expression_filter[129]{},hotkey_filter[129]{};
struct ShortcutPress {unsigned chord;bool cancel;std::chrono::steady_clock::time_point time;};
std::vector<ShortcutPress> shortcut_events;
GLFWwindow* shortcut_window=nullptr;
GLFWkeyfun previous_key_callback=nullptr;
GLFWwindowfocusfun previous_focus_callback=nullptr;
void shortcut_focus_callback(GLFWwindow* w,int focused){
    if(previous_focus_callback)previous_focus_callback(w,focused);
    if(!focused&&w==shortcut_window){shortcut_events.clear();recording_key.clear();}
}
void shortcut_callback(GLFWwindow* w,int key,int scan,int action,int mods){
    if(previous_key_callback)previous_key_callback(w,key,scan,action,mods);
    if(action!=GLFW_PRESS||w!=shortcut_window||!glfwGetWindowAttrib(w,GLFW_FOCUSED))return;
    const unsigned m=unsigned(mods)&15u;
    const bool cancel=key==GLFW_KEY_ESCAPE&&m==0;
    const auto chord=shortcut_encode(unsigned(key),m);
    if((chord||cancel)&&shortcut_events.size()<32)shortcut_events.push_back({chord,cancel,std::chrono::steady_clock::now()});
}
void shortcut_uninstall(){
    if(shortcut_window){glfwSetKeyCallback(shortcut_window,previous_key_callback);glfwSetWindowFocusCallback(shortcut_window,previous_focus_callback);}
    shortcut_window=nullptr;previous_key_callback=nullptr;previous_focus_callback=nullptr;shortcut_events.clear();recording_key.clear();
}
bool expression_open=false,expression_wait=false,global_local_fallback=false;
std::chrono::steady_clock::time_point expression_poll,expression_sent;
struct ExpressionTerm {std::string id;char blend='O';float value=0;};
struct ExpressionItem {std::string id;bool show;};
struct ExpressionDetail {float fade=.5f,fade_out=.5f;std::vector<ExpressionTerm> terms;std::vector<ExpressionItem> items;std::string group;bool protected_expression=false;};
std::map<std::string,ExpressionDetail> expression_details;
struct ExpressionDraft {bool open=false,preview=false,saving=false,selected_only=false;int tab=0;std::string key="new";char name[513]{},search[129]{};ExpressionDetail detail;std::chrono::steady_clock::time_point test_until{};};
ExpressionDraft expression_draft;
struct ExpressionManagementUi {std::string key,delete_hotkey;char group[161]{},hotkey[161]{};bool protected_expression=false,confirm_delete=false;};
ExpressionManagementUi expression_management_ui;

void expression_editor_close(){expression_preview_clear();expression_draft=ExpressionDraft{};}
void expression_reset(){expression_editor_close();expression_management_ui=ExpressionManagementUi{};expression_details.clear();expression_filter[0]=hotkey_filter[0]=0;expression_rows.clear();recording_key.clear();shortcut_events.clear();expression_dir.clear();expression_status.clear();global_status.clear();global_local_fallback=false;expression_wait=false;expression_open=false;expression_poll={};}
bool expression_send(const std::string& text){
    if(expression_dir.empty()||expression_wait||text.size()>16383)return false;
    auto base=expression_dir+"/expression-request";
    {std::ifstream pending(base+".tsv");if(pending)return false;}
    std::remove((expression_dir+"/expression-result.txt").c_str());
    {std::ofstream f(base+".tmp",std::ios::trunc);f<<text<<'\n';f.close();if(!f)return false;}
    if(std::rename((base+".tmp").c_str(),(base+".tsv").c_str())!=0)return false;
    expression_wait=true;expression_sent=std::chrono::steady_clock::now();expression_status.clear();return true;
}
bool expression_request(const char* action,const std::string& key,unsigned chord=0){
    std::string text=std::string(action)+"\t"+key;
    if(std::string(action)=="bind"||std::string(action)=="global")text+="\t"+std::to_string(chord);
    return expression_send(text);
}
void expression_editor_begin(const ExpressionRow* row,bool duplicate=false){
    expression_editor_close();auto& d=expression_draft;d.open=true;d.key=row&&!duplicate?row->key:"new";
    if(row){d.detail=expression_details.at(row->key);std::string n=row->name;
        const std::string suffix=".exp3.json";if(n.size()>=suffix.size()&&n.compare(n.size()-suffix.size(),suffix.size(),suffix)==0)n.resize(n.size()-suffix.size());
        if(duplicate)n+=" Copy";std::snprintf(d.name,sizeof(d.name),"%s",n.c_str());}
    expression_status.clear();recording_key.clear();shortcut_events.clear();
}
void expression_editor_draw(){
    auto& d=expression_draft;
    ImGui::TextUnformatted(d.key=="new"?"New expression":"Edit expression");
    ImGui::SetNextItemWidth(-1);ImGui::BeginDisabled(d.key!="new");ImGui::InputTextWithHint("##expression_name","Expression name",d.name,sizeof(d.name));ImGui::EndDisabled();
    const auto now=std::chrono::steady_clock::now();
    if(d.test_until!=std::chrono::steady_clock::time_point{}&&now>=d.test_until){expression_preview_clear();d.test_until={};}
    bool testing=d.test_until!=std::chrono::steady_clock::time_point{};
    ImGui::BeginDisabled(testing);
    ImGui::TextUnformatted("Fade in (seconds)");ImGui::SetNextItemWidth(-1);
    if(ImGui::InputFloat("##fade_in",&d.detail.fade,.1f,1.f,"%.2f"))d.detail.fade=std::isfinite(d.detail.fade)?std::max(0.f,std::min(10.f,d.detail.fade)):.5f;
    ImGui::TextUnformatted("Fade out (seconds)");ImGui::SetNextItemWidth(-1);
    if(ImGui::InputFloat("##fade_out",&d.detail.fade_out,.1f,1.f,"%.2f"))d.detail.fade_out=std::isfinite(d.detail.fade_out)?std::max(0.f,std::min(10.f,d.detail.fade_out)):.5f;
    ImGui::Checkbox("Live preview (instant)",&d.preview);ImGui::EndDisabled();ImGui::SameLine();ImGui::Checkbox("Selected only",&d.selected_only);
    if(ImGui::RadioButton("Parameters",d.tab==0)){d.tab=0;d.search[0]=0;}ImGui::SameLine();if(ImGui::RadioButton("Items",d.tab==1)){d.tab=1;d.search[0]=0;}
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##expression_parameter_search",d.tab==0?"Search parameters":"Search items",d.search,sizeof(d.search));
    const auto& defs=editor_runtime().definitions();
    auto valid_draft=[&](){
        if((d.detail.terms.empty()&&d.detail.items.empty())||d.detail.terms.size()>256||d.detail.items.size()>32)return false;
        if(d.key!="new"&&std::any_of(expression_rows.begin(),expression_rows.end(),[&](const ExpressionRow& r){return !r.hotkey&&r.key==d.key&&r.active;}))return false;
        for(const auto& t:d.detail.terms){if(!std::isfinite(t.value)||std::abs(t.value)>1e6f||std::none_of(defs.begin(),defs.end(),[&](const parameters::Definition& def){return def.id==t.id;}))return false;}
        return true;
    };
    bool valid=valid_draft();
    if(d.key!="new"&&std::any_of(expression_rows.begin(),expression_rows.end(),[&](const ExpressionRow& r){return !r.hotkey&&r.key==d.key&&r.active;})){valid=false;ImGui::TextDisabled("Deactivate the expression before editing");}
    bool run_test=false;
    if(testing){if(ImGui::Button("Stop transition test")){expression_preview_clear();d.test_until={};testing=false;}}
    else{ImGui::BeginDisabled(!valid||d.saving);run_test=ImGui::Button("Test transition");ImGui::EndDisabled();}
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Fade in, hold for one second, then fade out. Uses the draft timings.");
    const float list_height=std::max(45.f,ImGui::GetContentRegionAvail().y-76.f*ui::scale());
    ImGui::BeginDisabled(testing);
    if(ImGui::BeginChild("##expression_parameters",ImVec2(0,list_height),ImGuiChildFlags_Borders)){
      if(d.tab==0){
      for(const auto& def:defs){
        auto it=std::find_if(d.detail.terms.begin(),d.detail.terms.end(),[&](const ExpressionTerm& t){return t.id==def.id;});
        bool selected=it!=d.detail.terms.end();if((d.selected_only&&!selected)||!ui_parameter_matches(def.id,d.search))continue;
        ImGui::PushID(def.id.c_str());bool on=selected;
        ImGui::BeginDisabled(!selected&&d.detail.terms.size()>=256);if(ImGui::Checkbox("##include",&on)){
          if(on){d.detail.terms.push_back({def.id,'O',def.initial});it=std::prev(d.detail.terms.end());selected=true;}
          else{d.detail.terms.erase(it);selected=false;}
        }ImGui::EndDisabled();ImGui::SameLine();ImGui::TextWrapped("%s",def.id.c_str());
        if(selected){
          ImGui::TextDisabled("Min %.2f | Max %.2f | Default %.2f",def.minimum,def.maximum,def.initial);
          int mode=it->blend=='O'?0:it->blend=='A'?1:2;
          ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##blend",&mode,"Overwrite\0Add\0Multiply\0")) {it->blend=mode==0?'O':mode==1?'A':'M';it->value=mode==0?def.initial:mode==1?0.f:1.f;}
          const float span=std::max(1.f,def.maximum-def.minimum);const float lo=mode==0?def.minimum:mode==1?-span:-10.f,hi=mode==0?def.maximum:mode==1?span:10.f;
          ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("##value",&it->value,lo,hi,"%.3f");
        }ImGui::Separator();ImGui::PopID();
      }
      // Preserve imported terms missing from a revised rig until explicitly removed.
      for(auto it=d.detail.terms.begin();it!=d.detail.terms.end();){if(std::none_of(defs.begin(),defs.end(),[&](const parameters::Definition& def){return def.id==it->id;})){
          ImGui::PushID(it->id.c_str());ImGui::TextWrapped("Missing: %s",it->id.c_str());if(ImGui::SmallButton("Remove")){it=d.detail.terms.erase(it);ImGui::PopID();continue;}ImGui::PopID();}++it;}
      }else{
        const auto& scene=items::scene();
        if(scene.rows.empty())ImGui::TextDisabled("Import PNG/GIF in Items first.");
        for(const auto& item:scene.rows){
            const auto id=items::asset_id(item);if(id.empty())continue;
            auto it=std::find_if(d.detail.items.begin(),d.detail.items.end(),[&](const ExpressionItem& t){return t.id==id;});
            int mode=it==d.detail.items.end()?0:it->show?1:2;
            auto name=item.path.substr(item.path.find_last_of('/')+1);
            if((d.selected_only&&mode==0)||!ui_parameter_matches(name,d.search))continue;
            ImGui::PushID(id.c_str());ImGui::TextWrapped("%s",name.c_str());
            ImGui::TextDisabled("Scene: %s",item.visible?"Visible":"Hidden");
            ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##visibility",&mode,"Unchanged\0Show\0Hide\0")){
                if(it!=d.detail.items.end()){if(mode==0)d.detail.items.erase(it);else it->show=mode==1;}
                else if(mode!=0&&d.detail.items.size()<32)d.detail.items.push_back({id,mode==1});
            }
            ImGui::Separator();ImGui::PopID();
        }
        // A removed asset never silently binds to a different item with the same filename.
        for(auto it=d.detail.items.begin();it!=d.detail.items.end();){
            if(std::none_of(scene.rows.begin(),scene.rows.end(),[&](const items::Item& item){return items::asset_id(item)==it->id;})){
                ImGui::PushID(it->id.c_str());ImGui::TextWrapped("Missing item: %s",it->id.c_str());if(ImGui::SmallButton("Remove action")){it=d.detail.items.erase(it);ImGui::PopID();continue;}ImGui::PopID();
            }++it;
        }
      }
    }ImGui::EndChild();ImGui::EndDisabled();
    valid=valid_draft();
    // The draft is a local temporary expression with a short safety TTL, never saved or exposed to plugins.
    std::ostringstream terms;terms.imbue(std::locale::classic());terms<<std::setprecision(9);
    for(const auto& t:d.detail.terms)terms<<' '<<t.id<<' '<<t.blend<<' '<<t.value;
    for(const auto& t:d.detail.items)terms<<' '<<t.id<<" I "<<(t.show?1:0);
    if(run_test&&valid){
      d.preview=false;expression_preview_clear();
      const auto fi=std::lround(d.detail.fade*1000.f),fo=std::lround(d.detail.fade_out*1000.f);
      const auto hold=fi+1000;
      if(expression_preview("expr on2 editor-preview "+std::to_string(fi)+" "+std::to_string(hold)+" "+std::to_string(fo)+terms.str())){
        d.test_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(hold+fo+50);testing=true;
      }else expression_status="Could not start transition test";
    }
    if(!testing){if(d.preview&&valid&&!d.saving){if(!expression_preview("expr on editor-preview 0 500"+terms.str()))expression_preview_clear();}else expression_preview_clear();}
    ImGui::TextDisabled("%zu parameters | %zu items",d.detail.terms.size(),d.detail.items.size());
    ImGui::BeginDisabled(!valid||!d.name[0]||d.saving||testing);
    if(ImGui::Button("Save expression")){
      std::ostringstream req;req.imbue(std::locale::classic());req<<std::setprecision(9)<<"save-expression\t"<<d.key<<'\t'<<(d.key=="new"?d.name:"")<<'\t'<<d.detail.fade<<'\t'<<d.detail.fade_out;
      for(const auto& t:d.detail.terms)req<<'\t'<<t.id<<'\t'<<t.blend<<'\t'<<t.value;
      for(const auto& t:d.detail.items)req<<'\t'<<t.id<<"\tI\t"<<(t.show?1:0);
      if(expression_send(req.str())){d.saving=true;expression_preview_clear();}else expression_status="Could not send expression request";
    }ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel"))expression_editor_close();
}
bool broadcast_open=false,broadcast_focus=false,broadcast_ready=false,broadcast_pending=false;
UiBroadcast broadcast_active,broadcast_saved,broadcast_draft;
std::string broadcast_message;
void broadcast_popup(const ImVec2& sz){
    const auto dir=library_dir();if(dir.empty())return;
    // Process acknowledgement even if the settings window was closed.
    if(!broadcast_ready){std::ifstream f(dir+"/broadcast-current.tsv");if(f&&ui_read_broadcast(f,broadcast_active)){broadcast_ready=true;broadcast_saved=broadcast_draft=broadcast_active;}}
    if(broadcast_pending){
        std::ifstream f(dir+"/broadcast-result.txt");std::string reply;
        if(std::getline(f,reply)){
            f.close();std::remove((dir+"/broadcast-result.txt").c_str());broadcast_pending=false;
            if(reply=="ok"){
                broadcast_saved=broadcast_draft;
                broadcast_active.fps=broadcast_saved.fps;broadcast_active.stats=broadcast_saved.stats;
                apply_live_broadcast_settings(broadcast_saved.fps,broadcast_saved.stats);
                broadcast_message=broadcast_saved.restart_diff(broadcast_active)?"Saved. Restart Kawari to apply output and quality changes.":"Saved and applied.";
            }else broadcast_message=reply;
        }
    }
    broadcast_focus=false;if(!broadcast_open)return;
    if(!ui::begin_tool("Settings###Broadcast Settings",broadcast_open,sz,float(ui_bar_height(int(sz.y))),510.f,540.f)){ImGui::End();return;}
    broadcast_focus=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if(!broadcast_ready){ImGui::TextWrapped("Broadcast preferences unavailable. Rebuild Application Core.");ImGui::End();return;}
    auto& d=broadcast_draft;
    auto combo=[](const char* label,int& v,const int* vals,const char* const* labels,int n){int index=0;for(int i=0;i<n;++i)if(vals[i]==v)index=i; if(ImGui::Combo(label,&index,labels,n))v=vals[index];};
    ImGui::Text("Active output: %d x %d @ %d FPS",broadcast_active.width,broadcast_active.height,broadcast_active.bfps);
    ImGui::TextDisabled("Requested mask: %d | SSAA: %dx",broadcast_active.mask,broadcast_active.ssaa);
    ImGui::Separator();ImGui::BeginDisabled(broadcast_pending);
    ImGui::BeginChild("##settings_content",ImVec2(0,std::max(40.f,ImGui::GetContentRegionAvail().y-108.f*ui::scale())),ImGuiChildFlags_None);
    if(ImGui::BeginTabBar("##settings_tabs")){
    if(ImGui::BeginTabItem(ui::tr("Rendering###rendering","Рендеринг###rendering"))){
    ImGui::TextDisabled("Apply on save");
    const int render_rates[]={0,30,60,120,144};const char* render_labels[]={"Unlimited","30","60","120","144"};
    combo("Render FPS limit",d.fps,render_rates,render_labels,5);
    ImGui::Checkbox("Performance statistics in terminal",&d.stats);
    ImGui::Separator();ImGui::TextDisabled("Apply on next launch");
    const int masks[]={256,512,1024,2048};const char* mask_labels[]={"256","512","1024","2048"};
    combo("Clipping mask size",d.mask,masks,mask_labels,4);
    const int aa[]={1,2};const char* aa_labels[]={"1x (native)","2x (4x pixels)"};
    combo("Supersampling",d.ssaa,aa,aa_labels,2);
    ImGui::EndTabItem();}
    if(ImGui::BeginTabItem(ui::tr("Broadcast###broadcast","Трансляция###broadcast"))){
    ImGui::TextDisabled("Apply on next launch");
    const int widths[]={1280,1920,2560,3840,1080,720};const int heights[]={720,1080,1440,2160,1920,1280};
    const char* presets[]={"1280 x 720","1920 x 1080","2560 x 1440","3840 x 2160","1080 x 1920","720 x 1280","Custom"};
    int preset=6;for(int i=0;i<6;++i)if(d.width==widths[i]&&d.height==heights[i])preset=i;
    if(ImGui::Combo("Resolution preset",&preset,presets,7)&&preset<6){d.width=widths[preset];d.height=heights[preset];}
    ImGui::InputInt("Output width",&d.width,0);ImGui::InputInt("Output height",&d.height,0);
    const int output_rates[]={30,60,120,144};const char* output_labels[]={"30","60","120","144"};
    combo("Output FPS target",d.bfps,output_rates,output_labels,4);
    ImGui::EndTabItem();}
    if(ImGui::BeginTabItem(ui::tr("General###general","Общие###general"))){
    auto& appearance=ui::preferences();
    const int scales[]={85,100,115,130,150};const char* scale_labels[]={"85%","100%","115%","130%","150%"};
    combo(ui::tr("UI scale###ui_scale","Масштаб UI###ui_scale"),appearance.scale,scales,scale_labels,5);
    int language=appearance.russian?1:0;const char* languages[]={"English","Русский"};
    if(ImGui::Combo(ui::tr("Navigation language###ui_language","Язык навигации###ui_language"),&language,languages,2))appearance.russian=language==1;
    ImGui::TextWrapped("%s",ui::tr("Navigation and tab titles only; changes save automatically.","Только навигация и заголовки вкладок; сохраняется автоматически."));
    ImGui::Separator();
    ImGui::Checkbox("Start without preview",&d.no_preview);
    if(d.no_preview)ImGui::TextWrapped("Use --preview at launch to restore the interface.");
    ImGui::EndTabItem();}
    ImGui::EndTabBar();}
    ImGui::EndChild();
    if(d.fps!=0&&d.fps<d.bfps)ImGui::TextWrapped("Render limit is below output FPS; output cannot reach its target.");
    if(!d.valid())ImGui::TextWrapped("Resolution must be 320-7680 per dimension; 2x SSAA requires both dimensions <=3840.");
    if(d.restart_diff(broadcast_active))ImGui::TextDisabled("Output/quality changes require a restart.");
    ImGui::Separator();
    ImGui::BeginDisabled(!d.valid()||switch_pending);
    if(ImGui::Button("Save")){
        const auto tmp=dir+"/broadcast-save.tmp",dest=dir+"/broadcast-save.tsv";
        std::remove((dir+"/broadcast-result.txt").c_str());
        std::ofstream f(tmp,std::ios::trunc);f<<ui_write_broadcast(d);f.close();
        if(f&&std::rename(tmp.c_str(),dest.c_str())==0){broadcast_pending=true;broadcast_message="Saving...";}
        else{std::remove(tmp.c_str());broadcast_message="Cannot submit preferences.";}
    }
    ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Revert")){d=broadcast_saved;broadcast_message.clear();}
    ImGui::SameLine();if(ImGui::Button("Defaults")){d=UiBroadcast{};broadcast_message.clear();}
    ImGui::EndDisabled();
    if(!broadcast_message.empty())ImGui::TextWrapped("%s",broadcast_message.c_str());
    if(!(d==broadcast_saved))ImGui::TextDisabled("Unsaved changes");
    ImGui::TextDisabled("Launch arguments override saved preferences.");
    ImGui::End();
}
void physics_popup(const ImVec2& sz){
    physics_focus=false;if(!physics_open)return;
    if(!ui::begin_tool("Physics",physics_open,sz,float(ui_bar_height(int(sz.y))),520.f,550.f)){ImGui::End();return;}
    physics_focus=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    auto* editor=physics::active();
    if(!editor||!editor->available()){
        ImGui::TextWrapped("No editable physics in the loaded runtime.");
        if(editor&&!editor->status.empty())ImGui::TextWrapped("%s",editor->status.c_str());
        ImGui::End();return;
    }
    auto& config=editor->current;
    ImGui::Checkbox("Use physics",&config.enabled);
    ImGui::TextUnformatted("Physics strength");ImGui::SameLine();
    if(ImGui::SmallButton(physics_groups_open?"Hide groups":"Groups..."))physics_groups_open=!physics_groups_open;
    float strength_percent=config.strength*100.f;ImGui::SetNextItemWidth(-1);
    if(ImGui::SliderFloat("##strength",&strength_percent,0.f,200.f,"%.0f%%",ImGuiSliderFlags_AlwaysClamp))config.strength=strength_percent/100.f;
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Relative to the loaded model's imported physics scales.");
    const int fps_values[]={0,30,60,120};const char* fps_labels[]={"Model default","30 Hz","60 Hz","120 Hz"};
    int fps_index=config.fps==30?1:config.fps==60?2:config.fps==120?3:0;
    if(ImGui::Combo("Physics rate",&fps_index,fps_labels,4))config.fps=fps_values[fps_index];
    if(editor->original_fps>0)ImGui::TextDisabled("Exported rate: %.2f Hz",editor->original_fps);
    else ImGui::TextDisabled("Exported rate: variable");
    ImGui::Separator();
    if(physics_groups_open){
    ImGui::Text("Physics group strength (%zu)",config.groups.size());
    ImGui::SameLine();ImGui::Checkbox("Adjusted only",&physics_adjusted_only);
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##physics_filter","Search group",physics_filter,sizeof(physics_filter));
    ImGui::BeginChild("##physics_groups",ImVec2(0,std::max(50.f,ImGui::GetContentRegionAvail().y-68.f*ui::scale())),ImGuiChildFlags_Borders);
    for(size_t i=0;i<config.groups.size();++i){
        auto& group=config.groups[i];if(!ui_parameter_matches(group.name,physics_filter)&&!ui_parameter_matches(group.id,physics_filter))continue;
        if(physics_adjusted_only&&group.factor==1.f)continue;
        ImGui::PushID(int(i));ImGui::TextUnformatted(group.name.c_str());
        if(group.name!=group.id)ImGui::TextDisabled("%s",group.id.c_str());
        ImGui::BeginDisabled(!editor->has_angles(i));
        float group_percent=group.factor*100.f;
        ImGui::SetNextItemWidth(std::max(60.f,ImGui::GetContentRegionAvail().x-65.f));
        if(ImGui::SliderFloat("##group_factor",&group_percent,0.f,200.f,"%.0f%%",ImGuiSliderFlags_AlwaysClamp))group.factor=group_percent/100.f;
        ImGui::SameLine();if(ImGui::SmallButton("100%"))group.factor=1.f;
        ImGui::EndDisabled();if(!editor->has_angles(i))ImGui::TextDisabled("No angular outputs");
        ImGui::PopID();
    }
    ImGui::EndChild();
    }
    ImGui::BeginDisabled(!editor->can_save()||!editor->dirty());if(ImGui::Button("Save"))editor->store();ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(!editor->dirty());if(ImGui::Button("Revert"))editor->revert();ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("Reset"))editor->reset();
    if(!editor->status.empty())ImGui::TextWrapped("%s",editor->status.c_str());
    else if(!editor->can_save())ImGui::TextDisabled("No managed profile; live changes only");
    if(editor->dirty())ImGui::TextDisabled("Unsaved changes");
    ImGui::End();
}
void animations_popup(const ImVec2& sz){
    animations_focus=false;if(!animations_open)return;
    if(!ui::begin_tool("Animations",animations_open,sz,float(ui_bar_height(int(sz.y))),560.f,550.f)){ImGui::End();return;}
    animations_focus=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    auto* e=animation::active();
    if(!e){ImGui::TextDisabled("No active model");ImGui::End();return;}
    const auto& rows=e->rows;
    auto label=[&](int i){return i==animation::model_idle?std::string("Model default"):i==animation::no_idle?std::string("None"):animation::valid_index(i,rows)?rows[size_t(i)].file:std::string("Unknown");};
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##idle",("Idle: "+label(e->current.idle)).c_str())){
        if(ImGui::Selectable("Model default",e->current.idle==animation::model_idle))e->idle(animation::model_idle);
        if(ImGui::Selectable("None",e->current.idle==animation::no_idle))e->idle(animation::no_idle);
        for(size_t i=0;i<rows.size();++i){ImGui::PushID(int(i));if(ImGui::Selectable((rows[i].file+" ("+rows[i].group+")").c_str(),e->current.idle==int(i)))e->idle(int(i));ImGui::PopID();}
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("Speed",&e->current.speed,.25f,2.f,"%.2fx",ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextDisabled("Speed applies to motions without sound.");
    if(e->preview()>=0)ImGui::TextWrapped("Playing: %s",label(e->preview()).c_str());
    else if(e->playing()>=0)ImGui::TextWrapped("Idle: %s",label(e->playing()).c_str());
    else ImGui::TextDisabled("Idle mode");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##animation_filter","Search animations",animations_filter,sizeof(animations_filter));
    ImGui::BeginChild("##motion_list",ImVec2(0,std::max(40.f,ImGui::GetContentRegionAvail().y-145.f*ui::scale())),ImGuiChildFlags_Borders);
    if(rows.empty())ImGui::TextDisabled("No motions declared in the runtime manifest.");
    for(size_t i=0;i<rows.size();++i){
        const auto& row=rows[i];if(!ui_parameter_matches(row.group,animations_filter)&&!ui_parameter_matches(row.file,animations_filter))continue;
        ImGui::PushID(int(i));
        if(ImGui::Selectable(row.file.c_str(),animation_selected==int(i)))animation_selected=int(i);
        ImGui::TextDisabled("%s | %.2f s%s%s",row.group.c_str(),row.duration,row.loop?" | Native loop":"",row.sound?" | Sound":"");
        ImGui::PopID();
    }
    ImGui::EndChild();
    const bool selected=animation::valid_index(animation_selected,rows);
    ImGui::BeginDisabled(!selected);if(ImGui::Button("Play"))e->play(animation_selected);ImGui::SameLine();if(ImGui::Button("Set as Idle"))e->idle(animation_selected);ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("Stop preview"))e->stop();
    ImGui::Checkbox("Repeat preview",&e->repeat_preview);
    if(selected&&rows[size_t(animation_selected)].loop)ImGui::TextDisabled("Native looping motion: use Stop preview to return to Idle.");
    ImGui::BeginDisabled(!e->can_save()||!e->dirty());if(ImGui::Button("Save"))e->store();ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(!e->dirty());if(ImGui::Button("Revert"))e->revert();ImGui::EndDisabled();
    ImGui::SameLine();if(ImGui::Button("Reset"))e->reset();
    if(!e->status.empty())ImGui::TextWrapped("%s",e->status.c_str());
    else if(e->dirty())ImGui::TextDisabled("Unsaved changes");
    ImGui::End();
}
bool plugins_open=false,plugins_focus=false;
std::string plugins_dir,plugins_status;
struct PluginRow{std::string key,state,count,name,developer;};
std::vector<PluginRow> plugin_rows;
bool plugin_ready=false;
std::chrono::steady_clock::time_point plugin_poll;
// Twitch UI exchanges only public display state and explicit actions with the
// native helper. No OAuth access/refresh token or device secret enters ImGui.
struct TwitchReward {std::string id,title,cost,file,mode,duration;};
struct TwitchUi {
    std::string dir,status,note,login,code,url,model,modelName,reward,selectionKey,file,requestError;
    char client[129]{};bool enabled=false,ready=false,clientInitialized=false;
    int mode=0;float duration=60.f;
    std::vector<TwitchReward> rewards;std::vector<std::pair<std::string,std::string>> expressions;
    std::map<std::string,UiExpressionTimer> timers;
    std::chrono::steady_clock::time_point next{};
};
TwitchUi twitch_ui;
std::vector<std::string> twitch_fields(const std::string& line){std::vector<std::string> v;size_t pos=0;for(;;){auto end=line.find('\t',pos);v.push_back(line.substr(pos,end-pos));if(end==std::string::npos)break;pos=end+1;}return v;}
void twitch_panel(){
    auto& t=twitch_ui;const auto dir=library_dir();
    if(dir!=t.dir){t=TwitchUi{};t.dir=dir;}
    const auto now=std::chrono::steady_clock::now();
    if(now>=t.next){t.next=now+std::chrono::milliseconds(250);std::ifstream f(dir+"/twitch-ui.tsv");std::string line;
        t.ready=!dir.empty()&&bool(std::getline(f,line))&&line=="kawari-twitch-1";
        if(t.ready){t.rewards.clear();t.expressions.clear();t.code.clear();t.url.clear();t.model.clear();t.modelName.clear();t.login.clear();t.enabled=false;
            while(std::getline(f,line)){auto v=twitch_fields(line);if(v.empty())continue;
                if(v[0]=="S"&&v.size()==3){t.status=v[1];t.note=v[2];}
                else if(v[0]=="C"&&v.size()==4){if(!t.clientInitialized){if(!v[1].empty())std::snprintf(t.client,sizeof(t.client),"%s",v[1].c_str());t.clientInitialized=true;}t.login=v[2];t.enabled=v[3]=="1";}
                else if(v[0]=="D"&&v.size()==3){t.code=v[1];t.url=v[2];}
                else if(v[0]=="M"&&v.size()==3){t.model=v[1];t.modelName=v[2];}
                else if(v[0]=="E"&&v.size()==3)t.expressions.emplace_back(v[1],v[2]);
                else if(v[0]=="R"&&v.size()==7)t.rewards.push_back({v[1],v[2],v[3],v[4],v[5],v[6]});
            }
        }
        t.timers.clear();const auto expression_dir_now=expression_ui_directory();
        if(t.ready&&!expression_dir_now.empty()){std::ifstream timers(expression_dir_now+"/expression-ui.tsv");t.timers=ui_expression_timers(timers,t.model);}
    }
    if(!t.ready){ImGui::TextWrapped("Native Twitch bridge unavailable. Run scripts/build-twitch.sh and restart Kawari.");return;}
    auto request=[&](const std::string& text){if(t.dir.empty())return;auto base=t.dir+"/twitch-request";std::ifstream prev(base+".tsv");if(prev){t.requestError="Previous command pending";return;}std::ofstream out(base+".tmp",std::ios::trunc);out<<text<<'\n';out.close();if(!out||chmod((base+".tmp").c_str(),0600)!=0||std::rename((base+".tmp").c_str(),(base+".tsv").c_str())!=0){t.requestError="Cannot send command";return;}t.requestError.clear();};
    ImGui::TextUnformatted("Twitch");ImGui::TextWrapped("%s",t.status.c_str());if(!t.login.empty())ImGui::TextUnformatted(t.login.c_str());if(!t.note.empty())ImGui::TextWrapped("%s",t.note.c_str());
    if(t.status=="Unavailable")return;
    ImGui::BeginDisabled(t.enabled);ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##client_id","Twitch Client ID",t.client,sizeof(t.client));ImGui::EndDisabled();
    if(!t.enabled){if(ImGui::Button("Connect"))request(std::string("connect\t")+t.client);ImGui::SameLine();if(ImGui::Button("Forget login"))request("logout");}
    else if(ImGui::Button("Disconnect"))request("disconnect");
    if(!t.code.empty()){ImGui::Separator();ImGui::TextWrapped("Code: %s",t.code.c_str());if(ImGui::Button("Copy code"))ImGui::SetClipboardText(t.code.c_str());ImGui::SameLine();if(ImGui::Button("Open Twitch"))request("open");ImGui::TextWrapped("%s",t.url.c_str());}
    if(!t.requestError.empty())ImGui::TextWrapped("%s",t.requestError.c_str());
    if(t.status!="Connected")return;
    ImGui::Separator();ImGui::TextUnformatted("Channel Points");ImGui::SameLine();if(ImGui::SmallButton("Refresh"))request("refresh");
    if(t.model.empty()){ImGui::TextDisabled("Waiting for model");return;}ImGui::TextDisabled("Model: %s",t.modelName.c_str());
    if(t.rewards.empty()){ImGui::TextDisabled("No rewards available");return;}
    timespec ts{};const bool clock_ok=clock_gettime(CLOCK_MONOTONIC,&ts)==0;
    const uint64_t timer_now=uint64_t(ts.tv_sec)*1000+uint64_t(ts.tv_nsec)/1000000;
    bool timers_heading=false;
    for(const auto& r:t.rewards){
      if(r.mode!="timed"||r.file.empty()||!clock_ok)continue;
      const auto found=t.timers.find(r.file);if(found==t.timers.end())continue;
      const auto ms=ui_expression_remaining(found->second,timer_now);if(!ms)continue;
      if(!timers_heading){ImGui::TextUnformatted("Timed expressions");timers_heading=true;}
      const auto seconds=(ms+999)/1000;ImGui::TextWrapped("%s",r.title.c_str());ImGui::TextDisabled("%02llu:%02llu remaining",static_cast<unsigned long long>(seconds/60),static_cast<unsigned long long>(seconds%60));
    }
    if(timers_heading)ImGui::Separator();
    auto selected=std::find_if(t.rewards.begin(),t.rewards.end(),[&](const TwitchReward& r){return r.id==t.reward;});if(selected==t.rewards.end()){selected=t.rewards.begin();t.reward=selected->id;}
    ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##reward",selected->title.c_str())){for(auto& r:t.rewards){ImGui::PushID(r.id.c_str());if(ImGui::Selectable((r.title+"###reward").c_str(),r.id==t.reward))t.reward=r.id;ImGui::PopID();}ImGui::EndCombo();}
    selected=std::find_if(t.rewards.begin(),t.rewards.end(),[&](const TwitchReward& r){return r.id==t.reward;});if(selected==t.rewards.end())return;
    const std::string key=t.model+"\t"+t.reward;
    if(t.selectionKey!=key){t.selectionKey=key;t.file=selected->file;t.mode=selected->mode=="timed"?1:selected->mode=="on"?2:selected->mode=="off"?3:0;t.duration=60.f;try{if(selected->mode=="timed")t.duration=std::stof(selected->duration);}catch(...){} }
    if(!selected->cost.empty())ImGui::TextDisabled("Cost: %s points",selected->cost.c_str());
    ImGui::TextUnformatted("Expression");ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo("##reward_expression",t.file.empty()?"Select expression":t.file.c_str())){for(auto& e:t.expressions){ImGui::PushID(e.first.c_str());if(ImGui::Selectable((e.first+"###expression").c_str(),e.first==t.file))t.file=e.first;ImGui::PopID();}ImGui::EndCombo();}
    const char* labels[]={"Toggle","Timed","On","Off"};const char* modes[]={"toggle","timed","on","off"};ImGui::TextUnformatted("Activation mode");ImGui::SetNextItemWidth(-1);ImGui::Combo("##reward_mode",&t.mode,labels,4);
    if(t.mode==1){ImGui::TextUnformatted("Duration (seconds)");ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("##reward_duration",&t.duration,1.f,300.f,"%.0f");}
    const bool known=std::any_of(t.expressions.begin(),t.expressions.end(),[&](const std::pair<std::string,std::string>& e){return e.first==t.file;});
    ImGui::BeginDisabled(!known);if(ImGui::Button("Save binding"))request("bind\t"+key+"\t"+t.file+"\t"+modes[t.mode]+"\t"+std::to_string(t.duration));ImGui::EndDisabled();
    const bool saved_known=std::any_of(t.expressions.begin(),t.expressions.end(),[&](const std::pair<std::string,std::string>& e){return e.first==selected->file;});
    ImGui::SameLine();ImGui::BeginDisabled(!saved_known);if(ImGui::Button("Test saved"))request("test\t"+key);ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(selected->file.empty());if(ImGui::Button("Remove")){request("unbind\t"+key);t.file.clear();t.mode=0;}ImGui::EndDisabled();
    if(!selected->file.empty()){
      ImGui::TextWrapped("Saved: %s (%s)",selected->file.c_str(),selected->mode.c_str());
      if(!saved_known)ImGui::TextDisabled("Missing expression - remove or change binding");
    }
}
void plugins_popup(const ImVec2& sz){
    const auto dir=expression_ui_directory();
    if(dir!=plugins_dir){plugins_dir=dir;plugins_status.clear();plugin_rows.clear();plugin_ready=false;plugin_poll={};}
    const auto now=std::chrono::steady_clock::now();
    if(now>=plugin_poll){plugin_poll=now+std::chrono::milliseconds(200);plugin_rows.clear();
    std::ifstream f(dir.empty()?std::string():dir+"/plugins-ui.tsv");std::string line;
    plugin_ready=bool(std::getline(f,line))&&line=="kawari-plugins-1";
    if(plugin_ready)while(std::getline(f,line)){std::istringstream in(line);PluginRow r;
        if(std::getline(in,r.key,'\t')&&std::getline(in,r.state,'\t')&&std::getline(in,r.count,'\t')&&std::getline(in,r.name,'\t')&&std::getline(in,r.developer)&&r.key.size()==32)plugin_rows.push_back(std::move(r));}}
    const auto& rows=plugin_rows;std::string line;
    plugins_focus=false;if(!plugins_open)return;
    if(ui::begin_tool("Integrations###Plugins",plugins_open,sz,float(ui_bar_height(int(sz.y))),530.f,440.f)){
        plugins_focus=ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if(ImGui::BeginTabBar("##integrations_tabs")){
        if(ImGui::BeginTabItem(ui::tr("Plugins###plugins","Плагины###plugins"))){
        if(!plugin_ready)ImGui::TextDisabled("Waiting for API / Plugin Manager unavailable");
        else if(rows.empty())ImGui::TextDisabled("No plugin requests yet.");
        std::ifstream result(dir+"/plugins-result.txt");if(std::getline(result,line)){plugins_status=line;result.close();std::remove((dir+"/plugins-result.txt").c_str());}
        const auto request=[&](const PluginRow& r,const char* action){
            const auto base=dir+"/plugins-request";std::ifstream old(base+".tsv");if(old)return;
            std::ofstream out(base+".tmp",std::ios::trunc);out<<action<<'\t'<<r.key<<'\n';out.close();if(out&&std::rename((base+".tmp").c_str(),(base+".tsv").c_str())==0)plugins_status="Waiting...";else plugins_status="Cannot submit request";
        };
        for(const auto& r:rows){ImGui::PushID(r.key.c_str());ImGui::TextUnformatted(r.name.c_str());ImGui::TextDisabled("%s | %s | %s connected",r.developer.c_str(),r.state.c_str(),r.count.c_str());
            if(r.state=="Pending"||r.state=="Blocked"||r.state=="CLI allowed"){if(ImGui::Button(r.state=="CLI allowed"?"Keep permission":"Allow"))request(r,"allow");ImGui::SameLine();}
            if(r.state!="Blocked"&&ImGui::Button(r.state=="Pending"?"Deny":"Revoke"))request(r,"block");
            ImGui::Separator();ImGui::PopID();}
        if(!plugins_status.empty())ImGui::TextWrapped("%s",plugins_status.c_str());
        ImGui::EndTabItem();}
        if(ImGui::BeginTabItem(ui::tr("Twitch###twitch","Twitch###twitch"))){
            twitch_panel();
            ImGui::EndTabItem();}
        ImGui::EndTabBar();}
    }ImGui::End();
}
void expressions_popup(const ImVec2& sz,GLFWwindow* window){
    auto dir=expression_ui_directory();if(dir!=expression_dir){expression_reset();expression_dir=dir;}
    const auto now=std::chrono::steady_clock::now();
    if(now>=expression_poll&&!dir.empty()){
        expression_poll=now+std::chrono::milliseconds(150);
        std::ifstream f(dir+"/expression-ui.tsv");if(f){std::vector<ExpressionRow> rows;std::map<std::string,ExpressionDetail> details;std::string line;global_status.clear();global_local_fallback=false;
          while(std::getline(f,line)){std::istringstream in(line);in.imbue(std::locale::classic());std::string type,key,chord,active,name;
            if(!std::getline(in,type,'\t')||!std::getline(in,key,'\t'))continue;
            if(type=="X"){std::string flag,group;if(std::getline(in,flag,'\t')){std::getline(in,group);details[key].group=group;details[key].protected_expression=flag=="1";}continue;}
            if(type=="D"){float fi,fo;if(in>>fi>>fo&&std::isfinite(fi)&&std::isfinite(fo)){auto& d=details[key];d.fade=fi;d.fade_out=fo;}continue;}
            if(type=="I"){std::string id;int show;if(in>>id>>show&&parameters::item_id_ok(id)&&(show==0||show==1))details[key].items.push_back({id,show==1});continue;}
            if(type=="T"){ExpressionTerm t;if(in>>t.id>>t.blend>>t.value&&std::isfinite(t.value))details[key].terms.push_back(t);continue;}
            if(type=="G"){global_status=key;continue;}
            if(type=="L"){global_local_fallback=key=="1";continue;}
            if(type=="S"){for(auto& r:rows)if(r.hotkey&&r.key==key)r.global=true;continue;}
            if(type=="H"&&!std::getline(in,chord,'\t'))continue;
            if(!std::getline(in,active,'\t')||!std::getline(in,name)||(type!="H"&&type!="E"))continue;
            unsigned n=0;try{if(type=="H"){auto raw=std::stoul(chord);if(raw>0xfffffffful||!shortcut_normalize(unsigned(raw),n))continue;}}catch(...){continue;}
            rows.push_back({key,name,active=="1",type=="H",n});}
          expression_rows=std::move(rows);expression_details=std::move(details);}
        if(expression_wait){std::ifstream result(dir+"/expression-result.txt");if(std::getline(result,expression_status)){expression_wait=false;}
          else if(now-expression_sent>std::chrono::seconds(5)){expression_wait=false;expression_status="Request timed out";}
          if(!expression_wait&&expression_draft.saving){expression_draft.saving=false;if(expression_status=="OK")expression_editor_close();}}
    }
    if(!expression_open){recording_key.clear();expression_editor_close();expression_management_ui=ExpressionManagementUi{};}
    auto& io=ImGui::GetIO();
    const bool focused=glfwGetWindowAttrib(window,GLFW_FOCUSED)==GLFW_TRUE;
    const bool modal=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if(!focused||!expression_open||modal||parameter_focus||physics_focus||animations_focus||io.WantTextInput)recording_key.clear();
    if(focused&&!modal&&!expression_wait&&!expression_draft.open){
        for(const auto& event:shortcut_events){
            if(now-event.time>std::chrono::milliseconds(500))continue;
            if(!recording_key.empty()){
                if(event.cancel){recording_key.clear();expression_status="Recording cancelled";break;}
                if(!event.chord)continue;
                const bool duplicate=std::any_of(expression_rows.begin(),expression_rows.end(),[&](const ExpressionRow& r){return r.hotkey&&r.key!=recording_key&&r.chord==event.chord;});
                if(duplicate){expression_status="Shortcut already assigned";break;}
                if(expression_request("bind",recording_key,event.chord))recording_key.clear();
                break;
            }
            if(io.WantTextInput||ImGui::IsAnyItemActive()||parameter_focus||physics_focus||animations_focus||broadcast_focus||plugins_focus||event.cancel)continue;
            for(const auto& r:expression_rows)if(r.hotkey&&(!r.global||global_local_fallback)&&r.chord&&r.chord==event.chord){expression_request("trigger",r.key);break;}
            if(expression_wait)break;
        }
    }
    shortcut_events.clear();
    if(!expression_open)return;
    if(ui::begin_tool("Expressions & Hotkeys",expression_open,sz,float(ui_bar_height(int(sz.y))),520.f,500.f)){
      if(expression_rows.empty())ImGui::TextDisabled("No expressions or waiting for API");
      ImGui::BeginDisabled(expression_wait);
      if(expression_draft.open){expression_editor_draw();}
      else if(ImGui::BeginTabBar("##expression_tabs")){
      if(ImGui::BeginTabItem(ui::tr("Expressions###expressions","Выражения###expressions"))){
        recording_key.clear();
        if(ImGui::Button("New expression"))expression_editor_begin(nullptr);
        if(ImGui::Button("Deactivate all"))expression_request("deactivate-all","model");
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Keep protected customization expressions active");
        ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##expression_filter","Search expressions",expression_filter,sizeof(expression_filter));
        size_t matches=0;
        for(const auto& r:expression_rows){if(r.hotkey||!ui_parameter_matches(r.name,expression_filter))continue;++matches;
          ImGui::PushID(r.key.c_str());bool active=r.active;
          if(ImGui::Checkbox((r.name+"###expression-toggle").c_str(),&active))expression_request("toggle",r.key);
          const auto detail=expression_details.find(r.key);if(detail!=expression_details.end()){ImGui::TextDisabled("Fade in %.2f s | Fade out %.2f s",detail->second.fade,detail->second.fade_out);ImGui::TextDisabled("%zu parameters | %zu items",detail->second.terms.size(),detail->second.items.size());}
          ImGui::BeginDisabled(r.active||!expression_details.count(r.key));if(ImGui::SmallButton("Edit"))expression_editor_begin(&r);ImGui::EndDisabled();
          ImGui::SameLine();ImGui::BeginDisabled(!expression_details.count(r.key));if(ImGui::SmallButton("Duplicate"))expression_editor_begin(&r,true);ImGui::EndDisabled();
          auto& m=expression_management_ui;
          if(detail!=expression_details.end()){
            if(!detail->second.group.empty())ImGui::TextWrapped("Group: %s",detail->second.group.c_str());
            if(detail->second.protected_expression)ImGui::TextDisabled("Protected customization");
            if(ImGui::SmallButton(m.key==r.key?"Close management":"Manage")){
              if(m.key==r.key)m=ExpressionManagementUi{};
              else{m=ExpressionManagementUi{};m.key=r.key;m.protected_expression=detail->second.protected_expression;std::snprintf(m.group,sizeof(m.group),"%s",detail->second.group.c_str());auto label=r.name;const std::string suffix=".exp3.json";if(label.size()>=suffix.size()&&label.compare(label.size()-suffix.size(),suffix.size(),suffix)==0)label.resize(label.size()-suffix.size());std::snprintf(m.hotkey,sizeof(m.hotkey),"%s",label.c_str());}
            }
            if(m.key==r.key){
              ImGui::BeginDisabled(r.active);ImGui::TextUnformatted("Exclusive group");ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##group","Empty = independent",m.group,sizeof(m.group));ImGui::Checkbox("Protected customization",&m.protected_expression);
              if(ImGui::Button("Save rules"))expression_send("configure-expression\t"+r.key+"\t"+m.group+"\t"+(m.protected_expression?"1":"0"));ImGui::EndDisabled();
              ImGui::TextUnformatted("New hotkey");ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##new_hotkey","Hotkey name",m.hotkey,sizeof(m.hotkey));
              ImGui::BeginDisabled(!m.hotkey[0]);if(ImGui::Button("Create hotkey"))expression_send("new-hotkey\t"+r.key+"\t"+m.hotkey);ImGui::EndDisabled();
              ImGui::BeginDisabled(r.active);if(!m.confirm_delete){if(ImGui::SmallButton("Delete expression"))m.confirm_delete=true;}else{ImGui::TextWrapped("Remove this expression and its hotkeys from the profile? Twitch bindings must be removed separately.");if(ImGui::SmallButton("Confirm deletion")){if(expression_request("delete-expression",r.key))m=ExpressionManagementUi{};}ImGui::SameLine();if(ImGui::SmallButton("Cancel deletion"))m.confirm_delete=false;}ImGui::EndDisabled();
            }
          }
          ImGui::Separator();ImGui::PopID();}
        if(!matches)ImGui::TextDisabled("No matching expressions");
        ImGui::EndTabItem();}
      if(ImGui::BeginTabItem(ui::tr("Hotkeys###hotkeys","Клавиши###hotkeys"))){
      ImGui::SetNextItemWidth(-1);if(ImGui::InputTextWithHint("##hotkey_filter","Search hotkeys",hotkey_filter,sizeof(hotkey_filter)))recording_key.clear();
      if(std::any_of(expression_rows.begin(),expression_rows.end(),[](const ExpressionRow& r){return r.hotkey;})){
        if(ImGui::Button("Import VTS shortcuts")){recording_key.clear();shortcut_events.clear();expression_request("import-vts","model");}
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Fill unassigned hotkeys from VTube Studio. Existing bindings stay unchanged.");
        ImGui::Separator();
      }
      size_t matches=0;
      for(const auto& r:expression_rows){if(!r.hotkey||!ui_parameter_matches(r.name,hotkey_filter))continue;++matches;ImGui::PushID(r.key.c_str());
          ImGui::TextWrapped("%s",r.name.c_str());
          if(ImGui::SmallButton("Trigger"))expression_request("trigger",r.key);
          ImGui::SameLine();bool global=r.global;ImGui::BeginDisabled(!r.chord);if(ImGui::Checkbox("Global",&global)){recording_key.clear();expression_request("global",r.key,global?1:0);}ImGui::EndDisabled();
          const bool recording=recording_key==r.key;
          ImGui::BeginDisabled(r.global);
          if(ImGui::Button((std::string(recording?"Press shortcut...":shortcut_label(r.chord))+"###record-shortcut").c_str())){
              recording_key=r.key;expression_status.clear();shortcut_events.clear();
          }
          ImGui::EndDisabled();
          ImGui::SameLine();if(ImGui::SmallButton("Clear")){recording_key.clear();expression_request("bind",r.key,0);}
          if(recording){ImGui::SameLine();if(ImGui::SmallButton("Cancel"))recording_key.clear();}
          auto& deleting=expression_management_ui.delete_hotkey;
          if(deleting==r.key){if(ImGui::SmallButton("Confirm hotkey deletion")){recording_key.clear();if(expression_request("delete-hotkey",r.key))deleting.clear();}ImGui::SameLine();if(ImGui::SmallButton("Cancel deletion"))deleting.clear();}
          else if(ImGui::SmallButton("Delete hotkey")){recording_key.clear();deleting=r.key;}
          ImGui::Separator();ImGui::PopID();}
      if(!matches)ImGui::TextDisabled("No matching hotkeys");
      ImGui::EndTabItem();}
      ImGui::EndTabBar();}
      ImGui::EndDisabled();if(!global_status.empty())ImGui::TextWrapped("%s",global_status.c_str());if(!recording_key.empty())ImGui::TextDisabled("Press a key with any modifiers. Esc cancels.");if(!expression_status.empty())ImGui::TextWrapped("%s",expression_status.c_str());
    }ImGui::End();
    if(!expression_open){recording_key.clear();expression_editor_close();expression_management_ui=ExpressionManagementUi{};}
}
bool init(GLFWwindow* window,VkInstance inst,VkPhysicalDevice gpu,VkDevice dev,VkQueue queue,unsigned family,unsigned count,VkFormat fmt) {
    if(count<2) return false;
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr; ImGui::GetIO().LogFilename=nullptr;
    restore_workspace_preferences();
    ui::apply_theme();applied_ui_scale=ui::preferences().scale;
    ui::load_ui_font();
    app_icon_ready=app_icon::add_to_atlas(*ImGui::GetIO().Fonts,app_icon_uv_min,app_icon_uv_max);
    if(!ImGui_ImplGlfw_InitForVulkan(window,true)) {ImGui::DestroyContext();return false;}
    format=fmt; image_count=count; device=dev;
    ImGui_ImplVulkan_InitInfo i{};
    i.ApiVersion=VK_API_VERSION_1_3; i.Instance=inst; i.PhysicalDevice=gpu; i.Device=dev;
    i.QueueFamily=family;i.Queue=queue;i.MinImageCount=2;i.ImageCount=count;
    i.MSAASamples=VK_SAMPLE_COUNT_1_BIT;i.DescriptorPoolSize=8;
    i.UseDynamicRendering=true;i.PipelineRenderingCreateInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    i.PipelineRenderingCreateInfo.colorAttachmentCount=1;i.PipelineRenderingCreateInfo.pColorAttachmentFormats=&format;
    i.CheckVkResultFn=vk_result;
    if(!ImGui_ImplVulkan_Init(&i)) {ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext();device=VK_NULL_HANDLE;return false;}
    initialized=true;
    shortcut_window=window;previous_key_callback=glfwSetKeyCallback(window,shortcut_callback);
    previous_focus_callback=glfwSetWindowFocusCallback(window,shortcut_focus_callback);
    if(!ImGui_ImplVulkan_CreateFontsTexture()) {ui_shutdown();return false;}
    std::fprintf(stdout,"[ui] Studio workspace ready (Dear ImGui / Vulkan)\n");std::fflush(stdout);
    return true;
}
constexpr ImGuiWindowFlags flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoBringToFrontOnFocus;
bool items_open=false,items_drag=false,items_mouse=false,items_changed=false;
int item_selected=-1;char item_path[4096]{};
void items_popup(const ImVec2& sz){
    if(!items_open){items_drag=false;items_mouse=false;return;}
    auto& sc=items::scene();if(!ui::begin_tool("Items",items_open,sz,float(ui_bar_height(int(sz.y))),440.f,550.f)){ImGui::End();return;}
    if(!offscreen())ImGui::TextWrapped("Items require offscreen/broadcast rendering.");
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##item_path","Absolute PNG or GIF path",item_path,sizeof(item_path));
    if(ImGui::Button("Import")&&sc.rows.size()<32){items::Item item;if(items::import_file(item_path,item,sc.error)){sc.rows.push_back(item);item_selected=int(sc.rows.size())-1;sc.persist();item_path[0]=0;}}
    ImGui::SameLine();ImGui::TextDisabled("%zu / 32",sc.rows.size());
    if(!sc.error.empty())ImGui::TextWrapped("%s",sc.error.c_str());
    if(ImGui::BeginChild("##items_list",ImVec2(0,110*ui::scale()),ImGuiChildFlags_Borders)){
        for(size_t j=0;j<sc.rows.size();++j){auto& it=sc.rows[j];auto name=it.path.substr(it.path.find_last_of('/')+1);ImGui::PushID(int(j));if(ImGui::Selectable((name+"###item").c_str(),item_selected==int(j))){item_selected=int(j);items_mouse=false;}ImGui::PopID();}
    }ImGui::EndChild();
    if(item_selected>=0&&size_t(item_selected)<sc.rows.size()){
        auto& it=sc.rows[size_t(item_selected)];bool change=false;change|=ImGui::Checkbox("Visible",&it.visible);ImGui::SameLine();change|=ImGui::Checkbox("Locked",&it.locked);
        bool attach=it.attached;if(ImGui::Checkbox("Attach to model root",&attach)){float x,y;camera_offset(x,y);items::attach(it,attach,camera_scale(),x,y);change=true;}
        change|=ImGui::Checkbox("Behind model",&it.behind);ImGui::TextDisabled("Root attachment follows pan and zoom, not mesh deformation.");
        ImGui::BeginDisabled(it.locked);change|=ImGui::DragFloat("X",&it.x,.005f,-10000,10000,"%.3f",ImGuiSliderFlags_AlwaysClamp);change|=ImGui::DragFloat("Y",&it.y,.005f,-10000,10000,"%.3f",ImGuiSliderFlags_AlwaysClamp);change|=ImGui::DragFloat("Size",&it.size,.005f,.001f,10000,"%.3f",ImGuiSliderFlags_AlwaysClamp);change|=ImGui::SliderFloat("Rotation",&it.angle,-180,180,"%.0f deg");change|=ImGui::SliderFloat("Opacity",&it.opacity,0,1);ImGui::Checkbox("Drag selected in preview",&items_drag);ImGui::EndDisabled();
        if(it.locked)items_drag=false;
        if(ImGui::Button("Move down")&&item_selected>0){std::swap(sc.rows[size_t(item_selected)],sc.rows[size_t(item_selected-1)]);--item_selected;change=true;}
        ImGui::SameLine();if(ImGui::Button("Move up")&&size_t(item_selected+1)<sc.rows.size()){std::swap(sc.rows[size_t(item_selected)],sc.rows[size_t(item_selected+1)]);++item_selected;change=true;}
        if(ImGui::Button("Remove from scene")){sc.rows.erase(sc.rows.begin()+item_selected);item_selected=-1;items_mouse=false;change=true;}
        if(change)items_changed=true;
    }
    if(items_changed&&!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!ImGui::IsAnyItemActive()){sc.persist();items_changed=false;}
    ImGui::End();
}
struct Tool {const char* label;const char* title;bool* open;float width;bool floating;};
Tool tools[]={
    {"Parameters","Parameters###Parameter Editor",&parameter_open,720,false},
    {"Physics","Physics",&physics_open,520,false},
    {"Animations","Animations",&animations_open,560,false},
    {"Expressions","Expressions & Hotkeys",&expression_open,520,false},
    {"Integrations","Integrations###Plugins",&plugins_open,530,false},
    {"Settings","Settings###Broadcast Settings",&broadcast_open,510,false},
    {"Items","Items",&items_open,440,false}
};
constexpr int tool_count=sizeof(tools)/sizeof(tools[0]);
int selected_tool=-1;
bool stream_hidden[tool_count]{};
void restore_workspace_preferences(){
    if(ui_prefs_loaded)return;
    ui_prefs_loaded=true;auto& p=ui::preferences();
    if(!ui::read_preferences(ui::preferences_path(),p))std::fprintf(stderr,"[ui] Invalid or unsafe ui.conf; using defaults.\n");
    ui_prefs_saved=ui::encode_preferences(p);ui_prefs_candidate=ui_prefs_saved;
    ui_compact_bars()=p.stream;ui_fill_preview()=p.fill;
    for(int i=0;i<tool_count;++i){auto& t=tools[i];const auto& v=p.tools[size_t(i)];t.width=float(v.width);t.floating=v.floating;*t.open=v.open&&!p.stream;stream_hidden[i]=v.open&&p.stream;if(v.open)selected_tool=i;}
}
void persist_workspace_preferences(bool force){
    if(!ui_prefs_loaded)return;
    auto& p=ui::preferences();p.stream=ui_compact_bars();p.fill=ui_fill_preview();
    for(int i=0;i<tool_count;++i){auto& v=p.tools[size_t(i)];const auto& t=tools[i];v.width=int(t.width);v.floating=t.floating;v.open=p.stream?stream_hidden[i]:*t.open;}
    auto data=ui::encode_preferences(p);const auto now=std::chrono::steady_clock::now();
    if(data!=ui_prefs_candidate){ui_prefs_candidate=data;ui_prefs_changed=now;}
    if(data==ui_prefs_saved||(!force&&now-ui_prefs_changed<std::chrono::milliseconds(600)))return;
    if(ui::write_preferences(ui::preferences_path(),p))ui_prefs_saved=data;
    else {ui_prefs_changed=now;if(force)std::fprintf(stderr,"[ui] Could not save ui.conf.\n");}
}
// The dock resize handle overlays the right edge of the preview.
void resize_dock(int dock,const ImVec2& sz){
    auto& a=ui_workspace();if(dock<0||a.right<=0)return;
    const float edge=sz.x-a.right;
    ImGui::SetNextWindowPos(ImVec2(edge,a.bar));ImGui::SetNextWindowSize(ImVec2(7.f,sz.y-2*a.bar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize,ImVec2(1,1));ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
    ImGui::Begin("##dock_resize",nullptr,flags|ImGuiWindowFlags_NoBackground);
    ImGui::InvisibleButton("##resize",ImGui::GetContentRegionAvail());
    const bool active=ImGui::IsItemActive(),hover=ImGui::IsItemHovered();
    if(active||hover){ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(edge,a.bar),ImVec2(edge+3,sz.y-a.bar),ImGui::GetColorU32(ImGuiCol_SeparatorActive));}
    if(active){tools[dock].width=std::max(320.f,std::min(3000.f,sz.x-ImGui::GetIO().MousePos.x));dragging=false;}
    ImGui::End();ImGui::PopStyleVar(3);
}
void close_other_docks(int except){for(int i=0;i<tool_count;++i)if(i!=except&&!tools[i].floating)*tools[i].open=false;}
int docked_tool(){for(int i=0;i<tool_count;++i)if(*tools[i].open&&!tools[i].floating)return i;return -1;}
void select_tool(int i){
    auto& t=tools[i];selected_tool=i;
    if(*t.open){*t.open=false;ui::focus_title().clear();}
    else{if(!t.floating)close_other_docks(i);*t.open=true;ui::focus_title()=t.title;}
    dragging=false;
}
void hide_tools(){
    for(int i=0;i<tool_count;++i){stream_hidden[i]=*tools[i].open;*tools[i].open=false;}
    ui::focus_title().clear();dragging=false;
}
void restore_tools(){for(int i=0;i<tool_count;++i){*tools[i].open=stream_hidden[i];stream_hidden[i]=false;}}
void toggle_placement(int i){
    auto& t=tools[i];selected_tool=i;t.floating=!t.floating;
    if(!t.floating)close_other_docks(i);
    ui::focus_title()=t.title;dragging=false;
}
void navigation(const ImVec2& sz,const UiWorkspace& a){
    if(a.left<=0)return;
    ui::begin_chrome_at("##studio_navigation",0,a.bar,a.left,sz.y-2*a.bar,flags&~(ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse));
    const auto button_size=ImVec2(-1,34*ui::scale());
    if(ui::tool_button("##models_nav",library_open,button_size))library_requested=true;
    ui::navigation_icon(0);if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",ui::tr("Models","Модели"));
    ImGui::Separator();
    for(int i=0;i<tool_count;++i){
        auto& t=tools[i];std::string label=ui::preferences().russian?(i==3?"Выражения":ui::tool_ru(i)):t.label;
        if((i==0&&(editor_dirty()||draft_changed))||(i==1&&physics_dirty())||(i==2&&animation_dirty()))label+=" *";
        if(i==4){auto count=std::count_if(plugin_rows.begin(),plugin_rows.end(),[](const PluginRow& r){return r.state=="Pending";});if(count)label+=" ("+std::to_string(count)+")";}
        ImGui::PushID(i);
        if(ui::tool_button("##nav_tool",*t.open,button_size))select_tool(i);
        ui::navigation_icon(i+1==7?8:i+1,label!= (ui::preferences().russian?(i==3?"Выражения":ui::tool_ru(i)):t.label));
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",label.c_str());
        if(ImGui::BeginPopupContextItem("##placement")){
            if(ImGui::MenuItem(t.floating?ui::tr("Dock","Закрепить"):ui::tr("Float","Открепить"),nullptr,false,*t.open))toggle_placement(i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    if(selected_tool>=0&&*tools[selected_tool].open){
        auto& t=tools[selected_tool];
        if(ui::tool_button("##placement_button",!t.floating,button_size))toggle_placement(selected_tool);
        ui::navigation_icon(7);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s %s",t.floating?ui::tr("Dock","Закрепить"):ui::tr("Float","Открепить"),ui::preferences().russian?ui::tool_ru(selected_tool):t.label);
    }
    ImGui::End();
}

}
bool ui_begin(GLFWwindow* window,VkInstance inst,VkPhysicalDevice gpu,VkDevice dev,VkQueue queue,unsigned family,unsigned count,VkFormat fmt,VkExtent2D extent,int parameter_count) {
    frame_ready=false;
    if(!ui_requested()) return false;
    if(initialized && (format!=fmt || image_count!=count)) ui_shutdown();
    if(!initialized && !init(window,inst,gpu,dev,queue,family,count,fmt)) {
        ui_available()=false;std::fprintf(stderr,"[ui] Initialization failed; using classic preview\n");return false;
    }
    ImGui_ImplVulkan_NewFrame();ImGui_ImplGlfw_NewFrame();
    auto& io=ImGui::GetIO();
    const auto sz=io.DisplaySize;
    if(sz.x<=0 || sz.y<=0 || !extent.width || !extent.height) return false;
    // The acquired swapchain extent, not a racing GLFW resize, bounds scissors.
    io.DisplayFramebufferScale=ImVec2(float(extent.width)/sz.x,float(extent.height)/sz.y);
    if(applied_ui_scale!=ui::preferences().scale){ui::apply_theme();applied_ui_scale=ui::preferences().scale;}
    ImGui::NewFrame();
    if(toggle_fill){ui_fill_preview()=!ui_fill_preview();toggle_fill=false;dragging=false;}
    if(toggle_compact){ui_compact_bars()=!ui_compact_bars();toggle_compact=false;dragging=false;if(ui_compact_bars())hide_tools();else restore_tools();}
    const auto dir=expression_ui_directory();if(dir!=expression_dir){expression_reset();expression_dir=dir;}
    const auto output=output_size(int(sz.x),int(sz.y));
    const auto canvas=render_size(int(sz.x),int(sz.y));
    const auto initial=ui_workspace_layout(sz.x,sz.y,ui_compact_bars(),0,ui::scale());
    const float bar=initial.bar;
    ui::begin_chrome("##studio_header",0,sz.x,bar,flags);
    if(app_icon_ready){
        const float side=ImGui::GetFrameHeight();
        ImGui::Image(io.Fonts->TexID,ImVec2(side,side),app_icon_uv_min,app_icon_uv_max);
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Kawari");
    ImGui::SameLine();if(ImGui::Button(ui_compact_bars()?ui::tr("Studio###compact","Студия###compact"):ui::tr("Stream###compact","Стрим###compact")))toggle_compact=true;
    if(sz.x>=430){ImGui::SameLine();ImGui::TextDisabled("%s | %d parameters",model_name().c_str(),parameter_count);}
    ImGui::End();
    navigation(sz,initial);
    const int dock=docked_tool();
    const auto previous=ui_workspace();
    ui_workspace()=ui_workspace_layout(sz.x,sz.y,ui_compact_bars(),dock>=0?tools[dock].width:0,ui::scale());
    ui::dock_title()=dock>=0&&ui_workspace().right>0?tools[dock].title:"";
    if(previous.left!=ui_workspace().left||previous.right!=ui_workspace().right||previous.width!=sz.x||previous.height!=sz.y)dragging=false;
    // Tool drawing and mailbox polling are independent of navigation visibility.
    parameter_popup(sz);
    physics_popup(sz);
    animations_popup(sz);
    broadcast_popup(sz);
    plugins_popup(sz);
    expressions_popup(sz,window);
    items_popup(sz);
    resize_dock(dock,sz);
    ui::begin_chrome("##studio_footer",sz.y-bar,sz.x,bar,flags);
    if(ImGui::Button(ui::tr("Reset###camera_reset","Сброс###camera_reset"))){camera_reset();dragging=false;}
    ImGui::SameLine();if(ImGui::Button(ui::tr("Background###background","Фон###background")))cycle_background();
    ImGui::SameLine();if(ImGui::Button(ui_fill_preview()?ui::tr("Fill###preview_mode","Заполнить###preview_mode"):ui::tr("Fit###preview_mode","Вписать###preview_mode")))toggle_fill=true;
    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",ui::tr(
        "Preview only. Fit frames the whole output. Fill crops to the workspace. The dock can cover part of the image.",
        "Только для предпросмотра. «Вписать» показывает весь вывод. «Заполнить» обрезает его по области просмотра. Панель может перекрыть часть изображения."));
    if(sz.x>=360){ImGui::SameLine();ImGui::TextDisabled("Zoom %.0f%%",camera_scale()*100.f);}
    if(sz.x>=680){ImGui::SameLine();ImGui::TextDisabled("Output %d x %d | UI %.0f FPS",output.width,output.height,io.Framerate);}
    // Open and render the library popup from the same ID scope every frame.
    library_popup(sz);
    ImGui::End();
    // All mouse coordinates are logical window pixels, like the preview rect.
    const auto rect=preview_rect(int(sz.x),int(sz.y),canvas);
    const ImVec2 pos=io.MousePos;
    const bool inside=preview_contains(pos.x,pos.y,int(sz.x),int(sz.y),rect);
    const bool focused=glfwGetWindowAttrib(window,GLFW_FOCUSED)==GLFW_TRUE;
    if(ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel)){dragging=false;items_mouse=false;}
    if(!focused || !ImGui::IsMouseDown(ImGuiMouseButton_Left) || !ImGui::IsMousePosValid()) {dragging=false;if(items_mouse){items_mouse=false;if(items_changed){items::scene().persist();items_changed=false;}}}
    if(focused && inside && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel) && !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if(items_open&&items_drag&&item_selected>=0&&size_t(item_selected)<items::scene().rows.size()&&!items::scene().rows[size_t(item_selected)].locked){items_mouse=true;dragging=false;}else dragging=true;previous_mouse=pos;
    }
    if(items_mouse&&items_open&&item_selected>=0&&size_t(item_selected)<items::scene().rows.size()&&rect.w>0&&rect.h>0){auto& it=items::scene().rows[size_t(item_selected)];float z=it.attached?camera_scale():1.f;it.x=std::max(-10000.f,std::min(10000.f,it.x+2.f*(pos.x-previous_mouse.x)/rect.w/z));it.y=std::max(-10000.f,std::min(10000.f,it.y-2.f*(pos.y-previous_mouse.y)/rect.h/z));previous_mouse=pos;items_changed=true;}
    if(dragging && rect.w>0 && rect.h>0) {
        camera_pan(2.f*(pos.x-previous_mouse.x)/rect.w,-2.f*(pos.y-previous_mouse.y)/rect.h);
        previous_mouse=pos;
    }
    if(focused && inside && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel) && !io.WantCaptureMouse && io.MouseWheel!=0.f) {
        camera_scroll(io.MouseWheel,2.f*(pos.x-rect.x)/rect.w-1.f,1.f-2.f*(pos.y-rect.y)/rect.h);
    }
    ui_keyboard_capture()=parameter_focus || physics_focus || animations_focus || broadcast_focus || plugins_focus || io.WantCaptureKeyboard || ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if(items_changed&&!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!ImGui::IsAnyItemActive()){items::scene().persist();items_changed=false;}
    persist_workspace_preferences();
    ImGui::Render();frame_ready=true;return true;
}
void ui_draw(VkCommandBuffer cmd) {
    if(frame_ready) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),cmd);
    frame_ready=false;
}
void ui_model_changed(){
    items_mouse=false;
    // Do not reopen editors belonging to the previous model after Stream mode.
    for(int i=0;i<4;++i)stream_hidden[i]=false;
    expression_reset();
    animations_open=false;animations_focus=false;animations_filter[0]=0;animation_selected=-1;
    physics_open=false;physics_focus=false;physics_filter[0]=0;physics_groups_open=false;physics_adjusted_only=false;
    input_catalog.clear();input_catalog_ready=false;input_catalog_dir.clear();input_catalog_tick={};input_filter[0]=0;catalog_filter[0]=0;show_inputs=false;catalog_kind=0;
    stop_preview();parameter_open=false;parameter_focus=false;parameter_selected=-1;draft_changed=false;parameter_status.clear();parameter_filter[0]=0;input_id[0]=0;dragging=false;ui_keyboard_capture()=false;
}
void ui_shutdown() {
    frame_ready=false;dragging=false;stop_preview();parameter_focus=false;library_open=false;library_requested=false;ui_workspace()=UiWorkspace{};ui::dock_title().clear();ui::focus_title().clear();ui_keyboard_capture()=false;if(!initialized)return;
    if(items_changed){items::scene().persist();items_changed=false;}
    persist_workspace_preferences(true);
    vkDeviceWaitIdle(device);
    shortcut_uninstall();
    ImGui_ImplVulkan_Shutdown();ImGui_ImplGlfw_Shutdown();ImGui::DestroyContext();
    initialized=false;device=VK_NULL_HANDLE;format=VK_FORMAT_UNDEFINED;image_count=0;
}
}
