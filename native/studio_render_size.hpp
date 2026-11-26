#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "studio_ui_layout.hpp"
namespace studio {
struct CanvasSize { int width, height; };
inline bool fixed_canvas() {
    const char* s = std::getenv("STUDIO_BROADCAST");
    return s && std::strcmp(s,"1")==0;
}
inline int canvas_dimension(const char* key, int fallback) {
    const char* s = std::getenv(key);
    if (!s) return fallback;
    char* end = nullptr;
    const auto v = std::strtoul(s,&end,10);
    return end != s && !*end && v >= 320 && v <= 7680 ? static_cast<int>(v) : fallback;
}
inline CanvasSize output_size(int w, int h) {
    if (!fixed_canvas()) return {w,h};
    return {canvas_dimension("STUDIO_BROADCAST_WIDTH",1920),canvas_dimension("STUDIO_BROADCAST_HEIGHT",1080)};
}
// Optional 2x supersampling: keep the PipeWire stream at output_size.
inline CanvasSize render_size(int w,int h) {
    const auto out=output_size(w,h);
    const char* ssaa=std::getenv("STUDIO_SSAA");
    if (fixed_canvas() && ssaa && std::strcmp(ssaa,"2")==0 && out.width<=3840 && out.height<=3840)
        return {out.width*2,out.height*2};
    return out;
}
inline void apply_render_size(int& w,int& h) {
    const auto size=render_size(w,h); w=size.width; h=size.height;
}
struct PreviewRect { float x,y,w,h; };
inline PreviewRect preview_rect(int w,int h,CanvasSize canvas) {
    if (w<=0 || h<=0 || canvas.width<=0 || canvas.height<=0) return {0,0,0,0};
    const auto area=ui_preview_area(w,h);
    if(area.w<=0||area.h<=0)return {0,0,0,0};
    const bool fit_width = double(area.w)*canvas.height <= double(area.h)*canvas.width;
    const bool width_limited = ui_requested() && ui_fill_preview() ? !fit_width : fit_width;
    const float dw = width_limited ? area.w : float(double(area.h)*canvas.width/canvas.height);
    const float dh = width_limited ? float(double(area.w)*canvas.height/canvas.width) : area.h;
    return {area.x+(area.w-dw)*.5f,area.y+(area.h-dh)*.5f,dw,dh};
}
// Fit and Fill can extend behind the dock. Keep input in the visible area.
inline bool preview_contains(float px,float py,int w,int h,const PreviewRect& r) {
    const auto area=ui_preview_area(w,h);
    return r.w>0 && r.h>0 && px>=area.x && px<=area.input_right && py>=area.y && py<=area.y+area.h &&
        px>=r.x && px<=r.x+r.w && py>=r.y && py<=r.y+r.h;
}
// Inverse of the default (un-panned/un-zoomed) model projection.
inline bool preview_point(float px,float py,int w,int h,CanvasSize canvas,bool wide_model,float& x,float& y) {
    const auto r=preview_rect(w,h,canvas);
    if (!preview_contains(px,py,w,h,r)) return false;
    x=2.f*(px-r.x)/r.w-1.f; y=1.f-2.f*(py-r.y)/r.h;
    if (wide_model && canvas.width<canvas.height) y*=float(canvas.height)/canvas.width;
    else x*=float(canvas.width)/canvas.height;
    return true;
}
}
