// Independent user-session process. Only explicitly registered chords are grabbed.
#include "studio_ui_layout.hpp"
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>
#ifdef KAWARI_PORTAL
#include <gio/gio.h>
#endif
struct Binding {std::string id,label;unsigned chord;};
static void status(const std::string& s){std::cout<<"STATUS "<<s<<std::endl;}
static void event(const std::string& id){std::cout<<"EVENT "<<id<<std::endl;}
static KeySym symbol(unsigned k){
    if(k>=65&&k<=90)return XK_a+k-65;
    if(k>=290&&k<=314)return XK_F1+k-290;
    if(k>=320&&k<=329)return XK_KP_0+k-320;
    if(k>=330&&k<=336){const KeySym a[]={XK_KP_Decimal,XK_KP_Divide,XK_KP_Multiply,XK_KP_Subtract,XK_KP_Add,XK_KP_Enter,XK_KP_Equal};return a[k-330];}
    if(k>=256&&k<=269){const KeySym a[]={XK_Escape,XK_Return,XK_Tab,XK_BackSpace,XK_Insert,XK_Delete,XK_Right,XK_Left,XK_Down,XK_Up,XK_Page_Up,XK_Page_Down,XK_Home,XK_End};return a[k-256];}
    if(k>=280&&k<=284){const KeySym a[]={XK_Caps_Lock,XK_Scroll_Lock,XK_Num_Lock,XK_Print,XK_Pause};return a[k-280];}
    if(k==348)return XK_Menu;
    if(k>=32&&k<=126)return k;
    return NoSymbol;
}
static std::string trigger(unsigned chord){
    const unsigned m=(chord>>9)&15;std::string s;
    if(m&2)s+="CTRL+";
    if(m&1)s+="SHIFT+";
    if(m&4)s+="ALT+";
    if(m&8)s+="LOGO+";
    const char* k=XKeysymToString(symbol(chord&511));return k?s+k:std::string();
}
static int xerror=0;
static int on_xerror(Display*,XErrorEvent* e){xerror=e->error_code;return 0;}
static unsigned modifier(Display* d,KeySym a,KeySym b){
    unsigned out=0;auto* map=XGetModifierMapping(d);if(!map)return 0;
    for(int i=0;i<8;++i)for(int j=0;j<map->max_keypermod;++j){auto k=map->modifiermap[i*map->max_keypermod+j];if(k){auto s=XkbKeycodeToKeysym(d,k,0,0);if(s==a||s==b)out|=1u<<i;}}
    XFreeModifiermap(map);return out;
}
static int run_x11(const std::vector<Binding>& bindings){
    Display* d=XOpenDisplay(nullptr);if(!d){status("X11 display unavailable");return 1;}
    XSetErrorHandler(on_xerror);Bool repeat=False;XkbSetDetectableAutoRepeat(d,True,&repeat);
    const auto root=DefaultRootWindow(d);const unsigned num=modifier(d,XK_Num_Lock,XK_Num_Lock),scroll=modifier(d,XK_Scroll_Lock,XK_Scroll_Lock),locks=LockMask|num|scroll;
    const unsigned alt=modifier(d,XK_Alt_L,XK_Alt_R),super=modifier(d,XK_Super_L,XK_Super_R);
    std::map<std::pair<unsigned,unsigned>,std::string> registered;std::set<unsigned> down;
    for(const auto& b:bindings){const unsigned m=(b.chord>>9)&15;const auto key=XKeysymToKeycode(d,symbol(b.chord&511));
        if(!key||((m&4)&&!alt)||((m&8)&&!super)){status("Unsupported X11 shortcut: "+b.label);continue;}
        const unsigned mask=((m&1)?ShiftMask:0)|((m&2)?ControlMask:0)|((m&4)?alt:0)|((m&8)?super:0);
        if((mask&locks)||registered.count({key,mask})){status("Conflicting X11 shortcut: "+b.label);continue;}
        std::vector<unsigned> grabbed;bool ok=true;
        for(unsigned bits=locks;;bits=(bits-1)&locks){xerror=0;XGrabKey(d,key,mask|bits,root,False,GrabModeAsync,GrabModeAsync);XSync(d,False);if(xerror){ok=false;break;}grabbed.push_back(mask|bits);if(!bits)break;}
        if(!ok){for(auto x:grabbed)XUngrabKey(d,key,x,root);XSync(d,False);status("X11 shortcut unavailable: "+b.label);continue;}
        registered[{key,mask}]=b.id;
    }
    status("X11: "+std::to_string(registered.size())+"/"+std::to_string(bindings.size())+" registered");
    if(registered.empty()){XCloseDisplay(d);return 1;}
    while(true){XEvent e;XNextEvent(d,&e);if(e.type==MappingNotify){XRefreshKeyboardMapping(&e.xmapping);continue;}
        if(e.type==KeyPress){auto found=registered.find({e.xkey.keycode,e.xkey.state&0xffu&~locks});if(found!=registered.end()&&down.insert(e.xkey.keycode).second)event(found->second);}
        if(e.type==KeyRelease){if(!repeat&&XPending(d)){XEvent next;XPeekEvent(d,&next);if(next.type==KeyPress&&next.xkey.time==e.xkey.time&&next.xkey.keycode==e.xkey.keycode)continue;}down.erase(e.xkey.keycode);}
    }XCloseDisplay(d);return 1;
}
#ifdef KAWARI_PORTAL
static constexpr const char* bus="org.freedesktop.portal.Desktop";
static constexpr const char* path="/org/freedesktop/portal/desktop";
static constexpr const char* iface="org.freedesktop.portal.GlobalShortcuts";
static GVariant* call(GDBusConnection* c,const char* interface,const char* method,GVariant* args,bool* unavailable=nullptr){
    if(unavailable)*unavailable=false;
    GError* error=nullptr;auto* v=g_dbus_connection_call_sync(c,bus,path,interface,method,args,nullptr,G_DBUS_CALL_FLAGS_NONE,10000,nullptr,&error);
    if(error){
        if(unavailable)*unavailable=g_error_matches(error,G_DBUS_ERROR,G_DBUS_ERROR_UNKNOWN_INTERFACE)||g_error_matches(error,G_DBUS_ERROR,G_DBUS_ERROR_UNKNOWN_METHOD);
        status(std::string(method)+": "+error->message);g_error_free(error);
    }return v;
}
static GVariant* options(const std::string& token,const char* session=nullptr){GVariantBuilder b;g_variant_builder_init(&b,G_VARIANT_TYPE_VARDICT);g_variant_builder_add(&b,"{sv}","handle_token",g_variant_new_string(token.c_str()));if(session)g_variant_builder_add(&b,"{sv}","session_handle_token",g_variant_new_string(session));return g_variant_builder_end(&b);}
struct Response{GVariant* value=nullptr;std::string path;};
static void response_cb(GDBusConnection*,const gchar*,const gchar* object,const gchar*,const gchar*,GVariant* p,gpointer data){auto& r=*static_cast<Response*>(data);if(r.path==object&&!r.value)r.value=g_variant_ref(p);}
static GVariant* request(GDBusConnection* c,const char* method,GVariant* args,const std::string& token){
    status(std::string("Wayland: ")+method+" pending");
    std::string sender=g_dbus_connection_get_unique_name(c);sender.erase(0,1);for(auto& ch:sender)if(ch=='.')ch='_';
    Response r;r.path="/org/freedesktop/portal/desktop/request/"+sender+"/"+token;
    auto sub=g_dbus_connection_signal_subscribe(c,bus,"org.freedesktop.portal.Request","Response",nullptr,nullptr,G_DBUS_SIGNAL_FLAGS_NONE,response_cb,&r,nullptr);
    auto* v=call(c,iface,method,args);if(v){const gchar* returned=nullptr;g_variant_get(v,"(&o)",&returned);r.path=returned;g_variant_unref(v);
        auto end=std::chrono::steady_clock::now()+std::chrono::minutes(3);while(!r.value&&!g_dbus_connection_is_closed(c)&&std::chrono::steady_clock::now()<end){while(g_main_context_iteration(nullptr,FALSE)){}std::this_thread::sleep_for(std::chrono::milliseconds(20));}}
    g_dbus_connection_signal_unsubscribe(c,sub);if(!r.value){status(std::string(method)+": portal request timed out or failed");return nullptr;}
    guint32 code=0;GVariant* result=nullptr;g_variant_get(r.value,"(u@a{sv})",&code,&result);g_variant_unref(r.value);if(code){status(std::string(method)+": portal response "+std::to_string(code)+(code==1?" (cancelled)":" (backend error)"));auto* details=g_variant_print(result,TRUE);status(std::string(method)+" response details: "+details);g_free(details);g_variant_unref(result);return nullptr;}
    status(std::string(method)+": success");return result;
}
struct PortalState {std::string session;std::set<std::string> desired,allowed,down;bool closed=false;};
static void portal_bindings(PortalState& state,GVariant* entries){
    state.allowed.clear();state.down.clear();
    if(entries){GVariantIter iter;g_variant_iter_init(&iter,entries);const gchar* id;GVariant* props;
        while(g_variant_iter_next(&iter,"(&s@a{sv})",&id,&props)){const gchar* desc=nullptr;
            if(state.desired.count(id)&&g_variant_lookup(props,"trigger_description","&s",&desc)&&desc&&*desc){state.allowed.insert(id);status(std::string("Portal binding: ")+desc);}g_variant_unref(props);}}
    status("Wayland: "+std::to_string(state.allowed.size())+"/"+std::to_string(state.desired.size())+" registered");
}
static void portal_changed(GDBusConnection*,const gchar*,const gchar*,const gchar*,const gchar*,GVariant* p,gpointer data){auto& state=*static_cast<PortalState*>(data);const gchar* session;GVariant* entries=nullptr;g_variant_get(p,"(&o@a(sa{sv}))",&session,&entries);if(state.session==session)portal_bindings(state,entries);g_variant_unref(entries);}
static void portal_closed(GDBusConnection*,const gchar*,const gchar*,const gchar*,const gchar*,GVariant*,gpointer data){static_cast<PortalState*>(data)->closed=true;status("Portal session closed");}
static void portal_event(GDBusConnection*,const gchar*,const gchar*,const gchar*,const gchar* signal,GVariant* p,gpointer data){auto& s=*static_cast<PortalState*>(data);const gchar *session,*id;guint64 timestamp;GVariant* opts=nullptr;g_variant_get(p,"(&o&st@a{sv})",&session,&id,&timestamp,&opts);g_variant_unref(opts);if(s.session!=session||!s.allowed.count(id))return;if(std::string(signal)=="Activated"){if(s.down.insert(id).second)event(id);}else s.down.erase(id);}
static int run_portal(const std::vector<Binding>& bindings){
    GError* error=nullptr;auto* c=g_bus_get_sync(G_BUS_TYPE_SESSION,nullptr,&error);if(!c){status(error?error->message:"Session bus unavailable");if(error)g_error_free(error);return 1;}
    GVariantBuilder empty;g_variant_builder_init(&empty,G_VARIANT_TYPE_VARDICT);bool legacy=false;
    auto* registration=call(c,"org.freedesktop.host.portal.Registry","Register",g_variant_new("(s@a{sv})","ru.tesutoidclub.Kawari",g_variant_builder_end(&empty)),&legacy);
    if(registration){g_variant_unref(registration);status("Wayland: registered app identity ru.tesutoidclub.Kawari");}
    else if(legacy){status("Wayland: legacy portal without Registry.Register");}
    else {status("Wayland: app identity registration failed; run scripts/install-portal-identity.sh and check portal log");return 1;}
    auto* version=call(c,"org.freedesktop.DBus.Properties","Get",g_variant_new("(ss)",iface,"version"));
    if(version){auto* printed=g_variant_print(version,TRUE);status(std::string("GlobalShortcuts interface version: ")+printed);g_free(printed);g_variant_unref(version);}
    status("Wayland: waiting for portal");std::string token="kawari"+std::to_string(getpid())+"_"+std::to_string(g_random_int());auto* created=request(c,"CreateSession",g_variant_new("(@a{sv})",options(token+"c","kawari_global_shortcuts")),token+"c");if(!created)return 1;
    const gchar* session=nullptr;if(!g_variant_lookup(created,"session_handle","&s",&session)||!g_variant_is_object_path(session)){g_variant_unref(created);status("Invalid portal session");return 1;}PortalState state;state.session=session;g_variant_unref(created);for(const auto& b:bindings)state.desired.insert(b.id);
    status("Wayland session: "+state.session);
    g_dbus_connection_signal_subscribe(c,bus,"org.freedesktop.portal.Session","Closed",state.session.c_str(),nullptr,G_DBUS_SIGNAL_FLAGS_NONE,portal_closed,&state,nullptr);
    // List is diagnostic only: even existing bindings must be activated by BindShortcuts.
    auto* listed=request(c,"ListShortcuts",g_variant_new("(o@a{sv})",state.session.c_str(),options(token+"l")),token+"l");
    if(listed){auto* entries=g_variant_lookup_value(listed,"shortcuts",G_VARIANT_TYPE("a(sa{sv})"));status("Wayland: existing shortcut count "+std::to_string(entries?g_variant_n_children(entries):0));if(entries)g_variant_unref(entries);g_variant_unref(listed);}
    if(state.closed){status("Wayland: session closed before BindShortcuts");return 1;}
    GVariantBuilder list;g_variant_builder_init(&list,G_VARIANT_TYPE("a(sa{sv})"));for(const auto& b:bindings){auto t=trigger(b.chord);if(t.empty())continue;status("Wayland requested binding: "+b.id+" "+t);GVariantBuilder props;g_variant_builder_init(&props,G_VARIANT_TYPE_VARDICT);g_variant_builder_add(&props,"{sv}","description",g_variant_new_string(b.label.c_str()));g_variant_builder_add(&props,"{sv}","preferred_trigger",g_variant_new_string(t.c_str()));g_variant_builder_add(&list,"(s@a{sv})",b.id.c_str(),g_variant_builder_end(&props));}
    auto* bound=request(c,"BindShortcuts",g_variant_new("(o@a(sa{sv})s@a{sv})",state.session.c_str(),g_variant_builder_end(&list),"",options(token+"b")),token+"b");if(!bound)return 1;
    auto* entries=g_variant_lookup_value(bound,"shortcuts",G_VARIANT_TYPE("a(sa{sv})"));portal_bindings(state,entries);if(entries)g_variant_unref(entries);g_variant_unref(bound);
    for(const char* sig:{"Activated","Deactivated"})g_dbus_connection_signal_subscribe(c,bus,iface,sig,path,nullptr,G_DBUS_SIGNAL_FLAGS_NONE,portal_event,&state,nullptr);
    g_dbus_connection_signal_subscribe(c,bus,iface,"ShortcutsChanged",path,nullptr,G_DBUS_SIGNAL_FLAGS_NONE,portal_changed,&state,nullptr);
    while(!state.closed&&!g_dbus_connection_is_closed(c)){while(g_main_context_iteration(nullptr,FALSE)){}std::this_thread::sleep_for(std::chrono::milliseconds(10));}return 1;
}
#endif
int main(int argc,char** argv){
    auto parent=getppid();if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent||parent==1)return 1;
    if(argc==3&&std::string(argv[1])=="--trigger-format"){std::cout<<trigger(std::stoul(argv[2]))<<std::endl;return 0;}
    if(argc<4||(argc-1)%3)return 2;
    std::vector<Binding> bindings;
    for(int i=1;i<argc;i+=3){unsigned chord=0;try{auto raw=std::stoul(argv[i+1]);if(raw>0xfffffffful||!studio::shortcut_normalize(unsigned(raw),chord)||!chord)return 2;}catch(...){return 2;}bindings.push_back({argv[i],argv[i+2],chord});}
    const char* session=getenv("XDG_SESSION_TYPE");const bool wayland=(session&&std::string(session)=="wayland")||(!session&&getenv("WAYLAND_DISPLAY"));
    if(wayland){
#ifdef KAWARI_PORTAL
        return run_portal(bindings);
#else
        status("Wayland portal support not built (install glib2 development files)");return 1;
#endif
    }return run_x11(bindings);
}
