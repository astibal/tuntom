#pragma once
#include "ascon.hpp"
#include "x25519.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/random.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace tuntom::control_auth {
using Key = x25519::Bytes;
using Id = std::array<std::uint8_t,16>;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr std::size_t challenge_size=80, proof_size=88;
struct EntropyError : std::runtime_error {using std::runtime_error::runtime_error;};
inline void random(Key& key) {
    std::size_t at=0;
    while(at<key.size()) {
        const auto n=::getrandom(key.data()+at,key.size()-at,GRND_NONBLOCK);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)throw EntropyError("cannot generate control challenge");
        at+=static_cast<std::size_t>(n);
    }
}
inline Key parse_key(const std::string& text) {
    if(text.size()!=64)throw std::runtime_error("control key must be 64 hexadecimal digits");
    Key key{};
    for(std::size_t i=0;i<64;++i) {
        const auto n=std::string("0123456789abcdef").find(text[i]);
        if(n==std::string::npos)throw std::runtime_error("invalid control key");
        key[i/2]=static_cast<std::uint8_t>((key[i/2]<<4)|n);
    }
    if(key==Key{})throw std::runtime_error("zero control key");
    return key;
}
inline std::string hex(const Key& key) {
    std::string out;
    for(auto b:key){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}
    return out;
}
inline std::uint64_t number(const std::string& s) {
    if(s.empty() || s[0]=='-' || s[0]=='+')throw std::runtime_error("invalid control capability/level");
    const bool hex=s.size()>2 && s[0]=='0' && (s[1]=='x' || s[1]=='X');
    const auto digits=s.substr(hex?2:0);
    if(digits.empty() || digits.find_first_not_of(hex?"0123456789abcdefABCDEF":"0123456789")!=std::string::npos)
        throw std::runtime_error("invalid control capability/level");
    std::size_t used=0;const auto value=std::stoull(s,&used,hex?16:10);
    if(used!=s.size())throw std::runtime_error("invalid control capability/level");
    return value;
}
struct Grant {
    Key pub{};
    std::uint64_t caps=0;
    std::array<std::uint64_t,64> levels{};
    bool allows(std::uint64_t requested,std::uint64_t level)const {
        if((caps&requested)!=requested)return false;
        for(unsigned i=0;i<64;++i)if((requested&(std::uint64_t(1)<<i)) && levels[i]<level)return false;
        return true;
    }
};
struct SigningKey { Secret<32> secret; Grant grant; };
struct Config {
    std::map<Key,Grant> trusted;
    std::vector<std::shared_ptr<SigningKey>> signing;
    Key authority{};
    std::uint64_t caps=0,level=0;
    bool allow_trusted=false, allow_all=false;
    bool required()const{return allow_trusted;}
    bool enabled()const{return allow_trusted || allow_all;}
    // A record grants one level to its capability bitfield. Repeated records
    // for the same public key permit distinct levels for distinct capabilities.
    void load(const std::string& path,bool private_key) {
        std::ifstream file(path);
        if(!file)throw std::runtime_error("cannot open control key file: "+path);
        std::string line;bool any=false;std::size_t records=0;
        while(std::getline(file,line)) {
            WipeGuard wipe(line.data(),line.size());
            if(line.size()>512 || ++records>128)throw std::runtime_error("control key file too large");
            if(line.empty() || line[0]=='#')continue;
            std::istringstream in(line);std::string kind,key,caps_text,level_text,extra;
            if(!(in>>kind>>key>>caps_text>>level_text) || (in>>extra) || kind!=(private_key?"x25519-secret":"x25519"))
                throw std::runtime_error("expected x25519[-secret] HEX CAPS LEVEL in control key file");
            WipeGuard wipe_key(key.data(),key.size());
            auto bytes=parse_key(key);WipeGuard wipe_bytes(bytes.data(),bytes.size());
            const auto caps_value=number(caps_text),level_value=number(level_text);
            Grant* grant=nullptr;
            if(private_key) {
                auto signing_key=std::make_shared<SigningKey>(); signing_key->secret.bytes=bytes;
                x25519::public_key(signing_key->grant.pub,bytes);
                grant=&signing_key->grant;
                for(const auto& old:signing)if(old->grant.pub==signing_key->grant.pub){grant=&old->grant;break;}
                if(grant==&signing_key->grant)signing.push_back(std::move(signing_key));
            } else {
                // Reject low-order public keys at configuration time.
                Secret<32> test_secret,shared;test_secret.bytes[0]=1;
                if(!x25519::shared(shared.bytes,test_secret.bytes,bytes))throw std::runtime_error("invalid control public key");
                grant=&trusted[bytes];grant->pub=bytes;
            }
            grant->caps|=caps_value;
            for(unsigned i=0;i<64;++i)if(caps_value&(std::uint64_t(1)<<i))grant->levels[i]=std::max(grant->levels[i],level_value);
            any=true;
        }
        if(!any || file.bad())throw std::runtime_error("empty or unreadable control key file");
    }
};
inline bool option(Config& config,const std::string& option,int& i,int argc,char** argv) {
    if(option=="--allow-control-trusted"){config.allow_trusted=true;return true;}
    if(option=="--allow-control-all"){config.allow_all=true;return true;}
    if(option!="--control-trust-key" && option!="--control-authority-key" && option!="--control-require-authority" &&
       option!="--control-require-caps" && option!="--control-require-level")return false;
    if(++i>=argc)throw std::runtime_error(option+" requires a value");
    if(option=="--control-trust-key")config.load(argv[i],false);
    else if(option=="--control-authority-key")config.load(argv[i],true);
    else if(option=="--control-require-authority")config.authority=parse_key(argv[i]);
    else if(option=="--control-require-caps")config.caps=number(argv[i]);
    else config.level=number(argv[i]);
    return true;
}
inline void validate(const Config& c) {
    if(c.allow_trusted && c.allow_all)
        throw std::runtime_error("--allow-control-trusted and --allow-control-all are mutually exclusive");
    if((c.authority!=Key{} || c.caps || c.level) && c.trusted.empty())
        throw std::runtime_error("control requirements need --control-trust-key");
    if(c.authority!=Key{} && !c.trusted.count(c.authority))throw std::runtime_error("required control authority is not trusted");
}
inline constexpr const char* help =
    "  --allow-control-trusted       Allow CONTROL only from pinned authorities\n"
    "  --allow-control-all           DEBUG / AT OWN RISK: allow CONTROL without authority checks\n"
    "  --control-trust-key PATH      Pin X25519 authority public key/grants (repeatable)\n"
    "  --control-authority-key PATH  X25519 authority private key/grants (repeatable)\n"
    "  --control-require-authority HEX  Require this public key; otherwise peer selects\n"
    "  --control-require-caps MASK   Additional required capabilities (u64)\n"
    "  --control-require-level N     Minimum level for every required capability\n";
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
struct Challenge {
    Key n{},authority{};
    std::uint64_t caps=0,level=0;
    Bytes encode()const {
        Bytes b(challenge_size);std::copy(n.begin(),n.end(),b.begin());std::copy(authority.begin(),authority.end(),b.begin()+32);
        ascon::store_be64(b.data()+64,caps);ascon::store_be64(b.data()+72,level);return b;
    }
    static bool decode(const Bytes& b,Challenge& c) {
        if(b.size()!=challenge_size)return false;
        std::copy_n(b.begin(),32,c.n.begin());std::copy_n(b.begin()+32,32,c.authority.begin());
        c.caps=ascon::load_be64(b.data()+64);c.level=ascon::load_be64(b.data()+72);
        return c.n!=Key{};
    }
};
// CONTROL-KDF-v1 is a project-specific PRF derivation, not HKDF. AMAC is
// keyed by the first 128 bits of the DH output and absorbs ALL 256 DH bits,
// both public keys, the complete challenge, and the origin context. It relies
// on the existing AMAC PRF assumption; no standard-KDF/audit claim is made.
inline bool derive(ascon::key_type& out,const Key& secret,const Key& peer,const Challenge& challenge,
                   const Key& authority,const Id& origin) {
    Secret<32> dh;if(!x25519::shared(dh.bytes,secret,peer))return false;
    Secret<16> key;std::copy_n(dh.bytes.begin(),16,key.bytes.begin());
    const char label[]="TUNTOM-CONTROL-KDF-v1";
    Bytes input(label,label+sizeof(label));
    input.insert(input.end(),dh.bytes.begin(),dh.bytes.end());
    const auto bytes=challenge.encode();input.insert(input.end(),bytes.begin(),bytes.end());
    input.insert(input.end(),authority.begin(),authority.end());input.insert(input.end(),origin.begin(),origin.end());
    WipeGuard wipe(input.data(),input.size());ascon::mac(key.bytes,0,input.data(),input.size(),out);return true;
}
inline ascon::tag_type tag(const ascon::key_type& key,const Bytes& message,const Bytes& proof,bool reply) {
    const char label[]="TUNTOM-CONTROL-MAC-v1";
    Bytes input(label,label+sizeof(label));input.push_back(reply?1:0);
    input.insert(input.end(),proof.begin(),proof.begin()+72);
    input.insert(input.end(),message.begin(),message.end());
    ascon::tag_type result{};ascon::mac(key,0,input.data(),input.size(),result);return result;
}
struct Window {
    std::uint64_t high=0,bits=0;
    bool accept(std::uint64_t seq) {
        if(!seq)return false;
        if(seq>high){const auto d=seq-high;bits=d>=64?1:(bits<<d)|1;high=seq;return true;}
        const auto d=high-seq;if(d>=64 || (bits&(std::uint64_t(1)<<d)))return false;
        bits|=std::uint64_t(1)<<d;return true;
    }
};
class Auth {
    struct Session {
        Challenge challenge;Secret<32> secret;Secret<16> key;
        Key authority{};Id request{};Time expires{};
        Window rx;std::uint64_t tx=0;bool bound=false;
    };
    Config config_;Id origin_{};
    std::map<Key,std::unique_ptr<Session>> incoming_;
    std::map<Id,std::unique_ptr<Session>> outgoing_;
    std::map<Id,Key> responding_;
    std::unique_ptr<Session> offer_;
    Time next_challenge_{};
    static constexpr auto lifetime=std::chrono::minutes(2);
    static bool check(Session& s,const Bytes& message,const Bytes& proof,bool reply) {
        if(proof.size()!=proof_size)return false;
        const auto expected=tag(s.key.bytes,message,proof,reply);
        Key a{},b{};std::copy(expected.begin(),expected.end(),a.begin());std::copy_n(proof.begin()+72,16,b.begin());
        if(monocypher::crypto_verify32(a.data(),b.data()))return false;
        return s.rx.accept(ascon::load_be64(proof.data()+64));
    }
    static Bytes sign(Session& s,const Bytes& message,bool reply) {
        if(s.tx==UINT64_MAX)return {};
        Bytes p(proof_size);std::copy(s.challenge.n.begin(),s.challenge.n.end(),p.begin());
        std::copy(s.authority.begin(),s.authority.end(),p.begin()+32);ascon::store_be64(p.data()+64,++s.tx);
        const auto t=tag(s.key.bytes,message,p,reply);std::copy(t.begin(),t.end(),p.begin()+72);return p;
    }
public:
    void configure(const Config& c,const Id& origin={}) {config_=c;origin_=origin;}
    bool required()const{return config_.required();}
    bool enabled()const{return config_.enabled();}
    bool debug_all()const{return config_.allow_all;}
    bool signing()const{return !config_.signing.empty();}
    bool outgoing(const Id& id)const{return outgoing_.count(id)!=0;}
    void reset(){incoming_.clear();outgoing_.clear();responding_.clear();offer_.reset();next_challenge_=Time{};}
    void release_outgoing(const Id& id){outgoing_.erase(id);}
    void retire(const Id& id) {
        responding_.erase(id);
        for(auto i=incoming_.begin();i!=incoming_.end();)if(i->second->request==id)i=incoming_.erase(i);else ++i;
    }
    void forget(const Id& id){release_outgoing(id);retire(id);}
    void tick(Time now) {
        for(auto i=incoming_.begin();i!=incoming_.end();)if(now>=i->second->expires)i=incoming_.erase(i);else ++i;
        for(auto i=outgoing_.begin();i!=outgoing_.end();)if(now>=i->second->expires)i=outgoing_.erase(i);else ++i;
        for(auto i=responding_.begin();i!=responding_.end();)if(!incoming_.count(i->second))i=responding_.erase(i);else ++i;
        if(offer_ && now>=offer_->expires)offer_.reset();
    }
    Bytes challenge(const Id& request,std::uint64_t caps,Time now) {
        tick(now);
        // Bound unauthenticated DH work and memory; never evict a live challenge
        // merely because another request/retransmission arrived.
        if(!config_.enabled() || now<next_challenge_ || incoming_.size()>=32)return {};
        next_challenge_=now+std::chrono::milliseconds(20);
        auto s=std::make_unique<Session>();
        try{random(s->secret.bytes);}catch(const EntropyError&){return {};}
        x25519::public_key(s->challenge.n,s->secret.bytes);
        if(incoming_.count(s->challenge.n))return {}; // Fresh randomness, fail closed on collision.
        if(required()) {
            s->challenge.authority=config_.authority;
            if(config_.authority==Key{}){s->challenge.caps=caps|config_.caps;s->challenge.level=config_.level;}
        }
        s->request=request;s->expires=now+lifetime;
        auto bytes=s->challenge.encode();const auto n=s->challenge.n;incoming_.emplace(n,std::move(s));return bytes;
    }
    bool accept_challenge(const Id& request,const Bytes& bytes,Time now) {
        if(!enabled())return false;
        tick(now);Challenge challenge;if(!Challenge::decode(bytes,challenge))return false;
        // Duplicated challenges must not reset counters and enable replay.
        const auto old=outgoing_.find(request);
        if(old!=outgoing_.end() && old->second->challenge.n==challenge.n)return false;
        if(offer_ && offer_->challenge.n==challenge.n)return false;
        for(const auto& key:config_.signing) {
            if(challenge.authority!=Key{} ? key->grant.pub!=challenge.authority : !key->grant.allows(challenge.caps,challenge.level))continue;
            auto s=std::make_unique<Session>();s->challenge=challenge;s->authority=key->grant.pub;s->request=request;s->expires=now+lifetime;
            if(!derive(s->key.bytes,key->secret.bytes,challenge.n,challenge,s->authority,origin_))return false;
            if(request==Id{})offer_=std::move(s);
            else {if(outgoing_.size()>=16 && !outgoing_.count(request))return false;outgoing_[request]=std::move(s);}
            return true;
        }
        return false;
    }
    Bytes protect(const Id& request,const Bytes& message,bool reply) {
        if(!enabled())return {};
        if(reply) {
            const auto r=responding_.find(request);if(r==responding_.end())return {};
            const auto s=incoming_.find(r->second);return s==incoming_.end()?Bytes{}:sign(*s->second,message,true);
        }
        auto i=outgoing_.find(request);
        if(i==outgoing_.end() && offer_){offer_->request=request;i=outgoing_.emplace(request,std::move(offer_)).first;}
        return i==outgoing_.end()?Bytes{}:sign(*i->second,message,false);
    }
    bool verify_reply(const Id& request,const Bytes& message,const Bytes& proof) {
        if(!enabled())return false;
        const auto i=outgoing_.find(request);if(i==outgoing_.end() || proof.size()!=proof_size)return false;
        if(!std::equal(proof.begin(),proof.begin()+32,i->second->challenge.n.begin()) ||
           !std::equal(proof.begin()+32,proof.begin()+64,i->second->authority.begin()))return false;
        return check(*i->second,message,proof,true);
    }
    bool verify(const Id& request,const Bytes& message,const Bytes& proof,std::uint64_t caps,Key& principal) {
        if(!config_.enabled() || proof.size()!=proof_size)return false;
        Key n{},authority{};std::copy_n(proof.begin(),32,n.begin());std::copy_n(proof.begin()+32,32,authority.begin());
        const auto it=incoming_.find(n);if(it==incoming_.end())return false;auto& s=*it->second;
        if(s.request!=Id{} && s.request!=request)return false;
        // Debug mode accepts every authority (and unsigned commands in the
        // transaction layer). Proofs still get MAC/replay checking so an
        // authority can use the automatically advertised challenge.
        if(required()) {
            const auto grant=config_.trusted.find(authority);if(grant==config_.trusted.end())return false;
            if(s.challenge.authority!=Key{} && s.challenge.authority!=authority)return false;
            if(s.challenge.authority==Key{} && !grant->second.allows(s.challenge.caps,s.challenge.level))return false;
            if(!grant->second.allows(caps|config_.caps,config_.level))return false;
        }
        if(s.bound && s.authority!=authority)return false;
        if(!s.bound && !derive(s.key.bytes,s.secret.bytes,authority,s.challenge,authority,origin_))return false;
        if(!check(s,message,proof,false))return false;
        s.bound=true;s.authority=authority;s.request=request;s.secret.clear();principal=authority;responding_[request]=n;
        // Once one challenge wins, older alternatives for this request cannot
        // establish a second counter space or revive an already accepted frame.
        for(auto i=incoming_.begin();i!=incoming_.end();) {
            if(i->first!=n && i->second->request==request)i=incoming_.erase(i);else ++i;
        }
        return true;
    }
};
} // namespace tuntom::control_auth
