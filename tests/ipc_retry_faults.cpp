// One synthetic EAGAIN per record kind and fd in test executable children.
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
static bool injected(int fd, const void* data, size_t size) {
    static const bool target = [] {
        char path[4096]{};
        const auto n=readlink("/proc/self/exe",path,sizeof(path)-1);
        if(n<0) return false;
        const char* name=strrchr(path,'/'); name=name ? name+1 : path;
        return !strncmp(name,"tuntom",6) || !strncmp(name,"tomtom",6) || !strcmp(name,"divert_relay_fixture") || !strcmp(name,"adapter_reconnect_fixture");
    }();
    if(!target || fd<0 || fd>=4096 || size<8) return false;
    const auto* p=static_cast<const unsigned char*>(data);
    unsigned bit=0;
    if(p[0]==1) bit=1;
    else if(!memcmp(p,"TTX\x02",4) && p[4]>=16) bit=2;
    else if(!memcmp(p,"TTR\x01",4) && p[4]>=2 && p[4]<=4) bit=1U<<p[4];
    if(!bit) return false;
    static std::array<std::atomic<unsigned>,4096> seen{};
    if(seen[static_cast<size_t>(fd)].fetch_or(bit)&bit) return false;
    errno=EAGAIN; return true;
}
extern "C" ssize_t send(int fd,const void* data,size_t size,int flags) {
    static auto real=reinterpret_cast<ssize_t(*)(int,const void*,size_t,int)>(dlsym(RTLD_NEXT,"send"));
    return injected(fd,data,size) ? -1 : real(fd,data,size,flags);
}
extern "C" ssize_t sendmsg(int fd,const msghdr* msg,int flags) {
    static auto real=reinterpret_cast<ssize_t(*)(int,const msghdr*,int)>(dlsym(RTLD_NEXT,"sendmsg"));
    return msg->msg_iovlen && injected(fd,msg->msg_iov[0].iov_base,msg->msg_iov[0].iov_len) ? -1 : real(fd,msg,flags);
}
