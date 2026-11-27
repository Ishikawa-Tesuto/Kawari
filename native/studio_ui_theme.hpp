#pragma once
#include "imgui.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include "studio_ui_layout.hpp"
#include "studio_ui_preferences.hpp"

// Kawari's shared UI foundation. Only the preview overlay uses this style.
namespace studio { namespace ui {
inline ImVec4 color(unsigned rgb, float alpha=1.f) {
    return ImVec4(float((rgb>>16)&255)/255.f,float((rgb>>8)&255)/255.f,float(rgb&255)/255.f,alpha);
}
// Font atlas is populated before the Vulkan backend uploads its texture.
// Use installed fonts, not bundled binaries; never depend on the working directory.
inline bool load_ui_font() {
    auto& io=ImGui::GetIO();
    const char* paths[]={std::getenv("KAWARI_UI_FONT"),
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans[wdth,wght].ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    // Static storage is required by Dear ImGui 1.91 until atlas construction.
    static const ImWchar ranges[]={0x0020,0x024f,0x0400,0x052f,
        0x2000,0x206f,0x20a0,0x20cf,0x2de0,0x2dff,0xa640,0xa69f,0};
    for(const char* path:paths){
        if(!path||!*path)continue;
        std::ifstream f(path,std::ios::binary);if(!f)continue;f.close();
        auto* font=io.Fonts->AddFontFromFileTTF(path,14.f,nullptr,ranges);
        if(font&&io.Fonts->Build()){
            bool complete=font->FindGlyphNoFallback('A')&&font->FindGlyphNoFallback('a')&&
                font->FindGlyphNoFallback(0x0401)&&font->FindGlyphNoFallback(0x0451);
            for(ImWchar cp=0x0410;cp<=0x044f&&complete;++cp)
                complete=font->FindGlyphNoFallback(cp)!=nullptr;
            if(complete){io.FontDefault=font;std::fprintf(stdout,"[ui] Cyrillic font: %s\n",path);return true;}
        }
        io.FontDefault=nullptr;io.Fonts->Clear();
    }
    io.FontDefault=io.Fonts->AddFontDefault();
    std::fprintf(stderr,"[ui] No Cyrillic font found; install ttf-dejavu or set KAWARI_UI_FONT.\n");
    return false;
}
inline void apply_theme() {
    ImGui::GetStyle()=ImGuiStyle();
    ImGui::StyleColorsDark();
    auto& s=ImGui::GetStyle();
    s.WindowRounding=s.FrameRounding=s.ChildRounding=s.PopupRounding=s.ScrollbarRounding=s.GrabRounding=s.TabRounding=0.f;
    s.WindowPadding=ImVec2(12,12); s.FramePadding=ImVec2(8,5);
    s.ItemSpacing=ImVec2(6,4); s.ItemInnerSpacing=ImVec2(6,4);
    s.WindowBorderSize=s.ChildBorderSize=s.PopupBorderSize=s.FrameBorderSize=1.f;
    s.ScrollbarSize=10.f; s.GrabMinSize=8.f; s.IndentSpacing=16.f;
    s.WindowMinSize=ImVec2(32,32); s.WindowTitleAlign=ImVec2(0,.5f);
    auto* c=s.Colors;
    const auto base=color(0x15161b),surface=color(0x1e1f26),raised=color(0x292b35),border=color(0x3a3c48);
    const auto accent=color(0x9995c2),hover=color(0x414158),active=color(0x50516e),muted=color(0x90929f);
    c[ImGuiCol_Text]=color(0xe5e5ed); c[ImGuiCol_TextDisabled]=muted;
    c[ImGuiCol_WindowBg]=surface; c[ImGuiCol_ChildBg]=base; c[ImGuiCol_PopupBg]=surface;
    c[ImGuiCol_Border]=border; c[ImGuiCol_BorderShadow]=color(0,0);
    c[ImGuiCol_TitleBg]=raised; c[ImGuiCol_TitleBgActive]=raised; c[ImGuiCol_TitleBgCollapsed]=raised;
    c[ImGuiCol_MenuBarBg]=surface;
    c[ImGuiCol_FrameBg]=base; c[ImGuiCol_FrameBgHovered]=hover; c[ImGuiCol_FrameBgActive]=active;
    c[ImGuiCol_Button]=raised; c[ImGuiCol_ButtonHovered]=hover; c[ImGuiCol_ButtonActive]=active;
    c[ImGuiCol_Tab]=raised; c[ImGuiCol_TabHovered]=hover; c[ImGuiCol_TabSelected]=active;
    c[ImGuiCol_TabDimmed]=surface; c[ImGuiCol_TabDimmedSelected]=hover;
    c[ImGuiCol_TabSelectedOverline]=accent; c[ImGuiCol_TabDimmedSelectedOverline]=muted;
    c[ImGuiCol_Header]=active; c[ImGuiCol_HeaderHovered]=hover; c[ImGuiCol_HeaderActive]=active;
    c[ImGuiCol_CheckMark]=accent; c[ImGuiCol_SliderGrab]=accent; c[ImGuiCol_SliderGrabActive]=color(0xc6c2e2);
    c[ImGuiCol_Separator]=border; c[ImGuiCol_SeparatorHovered]=accent; c[ImGuiCol_SeparatorActive]=accent;
    c[ImGuiCol_ScrollbarBg]=base; c[ImGuiCol_ScrollbarGrab]=border; c[ImGuiCol_ScrollbarGrabHovered]=muted; c[ImGuiCol_ScrollbarGrabActive]=accent;
    c[ImGuiCol_ResizeGrip]=color(0x9995c2,.25f); c[ImGuiCol_ResizeGripHovered]=color(0x9995c2,.7f); c[ImGuiCol_ResizeGripActive]=accent;
    c[ImGuiCol_TextSelectedBg]=color(0x9995c2,.35f); c[ImGuiCol_ModalWindowDimBg]=color(0,.55f);
    c[ImGuiCol_PlotLines]=accent; c[ImGuiCol_PlotLinesHovered]=color(0xc6c2e2);
    s.ScaleAllSizes(scale());
    s.WindowBorderSize=s.ChildBorderSize=s.PopupBorderSize=s.FrameBorderSize=1.f;
    ImGui::GetIO().FontGlobalScale=scale();
}
// Preserve the existing toolbar heights; the larger padding is for tool windows.
inline void begin_chrome_at(const char* id,float x,float y,float w,float h,ImGuiWindowFlags flags) {
    ImGui::SetNextWindowPos(ImVec2(x,y)); ImGui::SetNextWindowSize(ImVec2(w,h));
    // Begin snapshots padding, min size and border; draw chrome with its own
    // background without leaking toolbar style into nested tool windows.
    auto& style=ImGui::GetStyle();
    const ImVec2 padding=style.WindowPadding, minimum=style.WindowMinSize;
    const float border=style.WindowBorderSize;
    const ImVec4 background=style.Colors[ImGuiCol_WindowBg];
    style.WindowPadding=ImVec2(6*scale(),5*scale()); style.WindowMinSize=ImVec2(1,1);
    style.WindowBorderSize=0.f; style.Colors[ImGuiCol_WindowBg]=color(0x15161b);
    ImGui::Begin(id,nullptr,flags);
    style.WindowPadding=padding; style.WindowMinSize=minimum;
    style.WindowBorderSize=border; style.Colors[ImGuiCol_WindowBg]=background;
}
inline void begin_chrome(const char* id,float y,float w,float h,ImGuiWindowFlags flags){begin_chrome_at(id,0,y,w,h,flags);}
// Only style the toolbar button, not the window it opens.
inline bool tool_button(const char* title,bool selected,ImVec2 size=ImVec2(0,0)) {
    if(selected)ImGui::PushStyleColor(ImGuiCol_Button,color(0x50516e));
    const bool clicked=ImGui::Button(title,size);
    if(selected)ImGui::PopStyleColor();
    return clicked;
}
// Keep actions on one line when their full labels fit at the current UI scale.
inline bool fits_after_item(float width){
    const float right=ImGui::GetCursorScreenPos().x+ImGui::GetContentRegionAvail().x;
    return ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+width<=right;
}
inline void next_action(const char* label){
    const float width=ImGui::CalcTextSize(label,nullptr,true).x+2.f*ImGui::GetStyle().FramePadding.x;
    if(fits_after_item(width))ImGui::SameLine();
}
inline void toolbar_status(const char* text){
    if(fits_after_item(ImGui::CalcTextSize(text).x)){
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s",text);
    }
}
inline bool primary_button(const char* label){
    ImGui::PushStyleColor(ImGuiCol_Button,color(0x535174));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,color(0x625d8a));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,color(0x494567));
    const bool pressed=ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return pressed;
}
inline void field_label(const char* text){
    ImGui::TextWrapped("%s",text);
    ImGui::SetNextItemWidth(-1);
}
inline bool choice_row(const char* name,const char* detail,bool selected,const char* tooltip=nullptr){
    const auto& style=ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(style.ItemSpacing.x,2.f*scale()));
    const bool pressed=ImGui::Selectable("##choice",selected,ImGuiSelectableFlags_NoAutoClosePopups,
                                        ImVec2(0,std::max(ImGui::GetFrameHeight(),28.f*scale())));
    ImGui::PopStyleVar();
    if(!ImGui::IsItemVisible())return pressed;
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    const float left=a.x+8.f*scale(),right=b.x-8.f*scale();
    const float y=(a.y+b.y-ImGui::GetTextLineHeight())*.5f;
    const float detail_width=ImGui::CalcTextSize(detail).x;
    const float detail_area=std::min(detail_width,std::max(0.f,(right-left)*.42f));
    const float name_end=detail_area>0?right-detail_area-8.f*scale():right;
    auto* draw=ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(left,a.y),ImVec2(std::max(left,name_end),b.y),true);
    draw->AddText(ImVec2(left,y),ImGui::GetColorU32(ImGuiCol_Text),name);
    draw->PopClipRect();
    if(detail_area>0){
        draw->PushClipRect(ImVec2(right-detail_area,a.y),ImVec2(right,b.y),true);
        draw->AddText(ImVec2(right-detail_area,y),ImGui::GetColorU32(selected?ImGuiCol_Text:ImGuiCol_TextDisabled),detail);
        draw->PopClipRect();
    }
    if((tooltip||ImGui::CalcTextSize(name).x>name_end-left||detail_width>detail_area)&&ImGui::BeginItemTooltip()){
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+std::min(420.f*scale(),ImGui::GetIO().DisplaySize.x*.8f));
        ImGui::TextWrapped("%s",tooltip?tooltip:name);
        if(!tooltip&&*detail)ImGui::TextWrapped("%s",detail);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return pressed;
}
// Small vector glyphs: no icon font, texture or network dependency.
inline void navigation_icon(int icon,bool attention=false){
    auto* dl=ImGui::GetWindowDrawList();const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    const float cx=(a.x+b.x)*.5f,cy=(a.y+b.y)*.5f,k=scale();
    const auto c=ImGui::GetColorU32(ImGuiCol_Text);const float stroke=std::max(1.f,1.5f*k);
    auto pt=[&](float x,float y){return ImVec2(cx+x*k,cy+y*k);};
    auto line=[&](float x,float y,float xx,float yy){dl->AddLine(pt(x,y),pt(xx,yy),c,stroke);};
    auto circle=[&](float x,float y,float r){dl->AddCircle(pt(x,y),r*k,c,20,stroke);};
    auto rect=[&](float x,float y,float xx,float yy){dl->AddRect(pt(x,y),pt(xx,yy),c,0,0,stroke);};
    switch(icon){
    case 0: circle(0,-4,3);dl->AddBezierCubic(pt(-7,7),pt(-7,-1),pt(7,-1),pt(7,7),c,stroke);break;
    case 1: for(int i=-1;i<=1;++i){const float x=float(i*6),y=float(i*4);line(x,-8,x,y-2);line(x,y+2,x,8);rect(x-2,y-2,x+2,y+2);}break;
    case 2: line(-9,0,-5,0);line(-5,0,-2,-7);line(-2,-7,2,7);line(2,7,5,0);line(5,0,9,0);break;
    case 3: rect(-8,-7,8,7);line(-2,-4,-2,4);line(-2,4,4,0);line(4,0,-2,-4);break;
    case 4: circle(0,0,8);circle(-3,-2,.8f);circle(3,-2,.8f);dl->AddBezierCubic(pt(-4,2),pt(-2,6),pt(2,6),pt(4,2),c,stroke);break;
    case 5: rect(-5,-3,5,3);line(-3,-8,-3,-3);line(3,-8,3,-3);line(0,3,0,8);break;
    case 6: circle(0,0,5);circle(0,0,2);for(int i=0;i<8;++i){const float t=float(i)*.78539816f;line(5*std::cos(t),5*std::sin(t),8*std::cos(t),8*std::sin(t));}break;
    case 7: rect(-7,-7,7,7);line(2,-7,2,7);break;
    case 8: rect(-8,-7,8,7);circle(3,-3,1.5f);line(-8,5,-2,-1);line(-2,-1,2,3);line(2,3,5,0);line(5,0,8,3);break;
    }
    if(attention)dl->AddCircleFilled(ImVec2(b.x-5*k,a.y+5*k),2.5f*k,ImGui::GetColorU32(color(0xdbb57c)),12);
}
inline void toggle_tool(const char* title,bool& open,bool& dragging) {
    if(tool_button(title,open)){open=!open;dragging=false;if(open)ImGui::SetNextWindowFocus();}
}
inline void place_tool(ImVec2 display,float bar,float width,float height,bool modal=false) {
    const float max_w=std::max(100.f,display.x-20.f);
    const float max_h=std::max(80.f,display.y-2*bar-12.f);
    const float w=std::min(width,max_w),h=std::min(height,max_h);
    ImGui::SetNextWindowSizeConstraints(ImVec2(100,80),ImVec2(max_w,max_h));
    ImGui::SetNextWindowSize(ImVec2(w,h),ImGuiCond_FirstUseEver);
    if(modal)ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    else ImGui::SetNextWindowPos(ImVec2(std::max(0.f,display.x-w-6.f),bar+6.f),ImGuiCond_FirstUseEver);
}
// Every tool uses the native ImGui title bar and close button. No collapsing.
// Caller must always End(), even when the close button changes open this frame.
inline std::string& dock_title(){static std::string title;return title;}
inline std::string& focus_title(){static std::string title;return title;}
inline bool begin_tool(const char* title,bool& open,ImVec2 display,float bar,float width,float height) {
    if(focus_title()==title){ImGui::SetNextWindowFocus();focus_title().clear();}
    const auto& a=ui_workspace();
    if(dock_title()==title&&a.right>0){
        const auto visible_title=std::string(title).substr(0,std::string(title).find("###"));
        const int index=tool_index(title);
        const auto id=(preferences().russian&&index>=0?std::string(tool_ru(index)):visible_title)+"###dock_"+visible_title;
        ImGui::SetNextWindowPos(ImVec2(a.width-a.right+7.f,a.bar));
        ImGui::SetNextWindowSize(ImVec2(a.right-7.f,a.height-2*a.bar));
        const bool visible=ImGui::Begin(id.c_str(),&open,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
        return visible&&open;
    }
    place_tool(display,bar,width,height);
    const int index=tool_index(title);
    auto label=window_label(title);
    if(index>=0){auto t=preferences().tools[size_t(index)];clamp_float(t,display.x,display.y,bar);
        if(t.w&&t.h){ImGui::SetNextWindowPos(ImVec2(float(t.x),float(t.y)),ImGuiCond_FirstUseEver);ImGui::SetNextWindowSize(ImVec2(float(t.w),float(t.h)),ImGuiCond_FirstUseEver);}}
    const bool visible=ImGui::Begin(label.c_str(),&open,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse);
    if(index>=0&&visible&&open){auto& t=preferences().tools[size_t(index)];const auto p=ImGui::GetWindowPos(),s=ImGui::GetWindowSize();t.x=int(p.x);t.y=int(p.y);t.w=int(s.x);t.h=int(s.y);}
    return visible&&open;
}
}}
