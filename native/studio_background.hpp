#pragma once
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
namespace studio {
inline bool no_preview_requested() {
    const char* s=std::getenv("STUDIO_NO_PREVIEW");
    return s && std::strcmp(s,"1")==0;
}
inline bool& background_frame_flag() { static bool value=false; return value; }
inline bool background_frame() { return background_frame_flag(); }
inline void set_background_frame(bool value) {
    // A compositor may alternate ready/unready images every frame. Avoid log spam;
    // the perf sampler reports the current mode instead.
    background_frame_flag()=value;
}
inline bool& pause_unfocused_preview() {
    static bool paused=[] {
        const char* setting=std::getenv("STUDIO_PAUSE_UNFOCUSED_PREVIEW");
        return setting&&std::strcmp(setting,"1")==0;
    }();
    return paused;
}
inline bool can_preview(bool hidden,bool visible,bool iconified,int width,int height,bool focused=true,bool pause_unfocused=false) {
    return !hidden && visible && !iconified && width>0 && height>0 && (focused||!pause_unfocused);
}
inline volatile std::sig_atomic_t& stop_flag() { static volatile std::sig_atomic_t flag=0; return flag; }
inline void stop_signal(int) { stop_flag()=1; }
inline void install_stop_signals() { stop_flag()=0; std::signal(SIGINT,stop_signal); std::signal(SIGTERM,stop_signal); }
inline bool stop_requested() { return stop_flag()!=0; }
}
