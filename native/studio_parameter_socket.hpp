#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace studio {
namespace parameters {
// Private per-instance socket; the directory is created mode 0700 by mkdtemp.
class Socket {
    int fd_=-1;
    std::string dir_,path_;
public:
    Socket()=default;
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
    ~Socket(){close_socket();}
    void close_socket() {
        if(fd_>=0) {::close(fd_);fd_=-1;}
        if(!path_.empty()) {::unlink(path_.c_str());path_.clear();}
        if(!dir_.empty()) {::unlink((dir_+"/parameters.tsv").c_str());::unlink((dir_+"/parameters.tmp").c_str());for(const char* f:{"expression-ui.tsv","expression-request.tsv","expression-request.tmp","expression-result.txt","obs-ui.tsv","obs-request.tsv","obs-request.tmp"})::unlink((dir_+"/"+f).c_str());::rmdir(dir_.c_str());dir_.clear();}
    }
    bool open_socket() {
        if(fd_>=0) return true;
        const char* base=std::getenv("XDG_RUNTIME_DIR");
        std::string pattern=std::string(base&&*base?base:"/tmp")+"/vtuber-params-XXXXXX";
        std::vector<char> buf(pattern.begin(),pattern.end());buf.push_back(0);
        char* d=::mkdtemp(buf.data());if(!d) return false;dir_=d;path_=dir_+"/input.sock";
        sockaddr_un addr{};addr.sun_family=AF_UNIX;
        if(path_.size()>=sizeof(addr.sun_path)){close_socket();return false;}
        std::memcpy(addr.sun_path,path_.c_str(),path_.size()+1);
        fd_=::socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd_<0||::bind(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){close_socket();return false;}
        std::fprintf(stdout,"[parameters] socket: %s\n",path_.c_str());std::fflush(stdout);return true;
    }
    template<class Handler> void drain(Handler handler) {
        if(fd_<0) return;
        char data[16384];
        for(int i=0;i<64;++i) {
            ssize_t n=::recv(fd_,data,sizeof(data),MSG_DONTWAIT|MSG_TRUNC);
            if(n<0) {if(errno==EINTR) continue;break;}
            if(n>0&&n<=static_cast<ssize_t>(sizeof(data))) handler(std::string(data,static_cast<size_t>(n)));
        }
    }
    const std::string& path() const {return path_;}
};
}
}
