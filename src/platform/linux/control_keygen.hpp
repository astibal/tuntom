#pragma once

#include "../../control_auth.hpp"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace tuntom::control_auth {

// Key provisioning is explicit and never overwrites existing files.
inline void keygen(const std::string& private_path,const std::string& public_path,
                   std::uint64_t caps,std::uint64_t level) {
    if(private_path==public_path)throw std::runtime_error("private and public key paths must differ");
    Secret<32> secret;random(secret.bytes);Key pub{};x25519::public_key(pub,secret.bytes);
    auto write_file=[](const std::string& path,const std::string& data,mode_t mode) {
        const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,mode);
        if(fd<0)throw std::runtime_error("cannot create control key file: "+path);
        std::size_t at=0;bool ok=true;
        while(at<data.size()) {
            const auto n=::write(fd,data.data()+at,data.size()-at);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){ok=false;break;}at+=static_cast<std::size_t>(n);
        }
        if(::close(fd)!=0)ok=false;
        if(!ok){::unlink(path.c_str());throw std::runtime_error("cannot write control key file: "+path);}
    };
    auto priv="x25519-secret "+hex(secret.bytes)+" "+std::to_string(caps)+" "+std::to_string(level)+"\n";
    WipeGuard wipe(priv.data(),priv.size());
    write_file(private_path,priv,0600);
    try{write_file(public_path,"x25519 "+hex(pub)+" "+std::to_string(caps)+" "+std::to_string(level)+"\n",0644);}
    catch(...){::unlink(private_path.c_str());throw;}
}
} // namespace tuntom::control_auth
