#include "../src/ipc/retry_queue.hpp"
#include <iostream>
#include <stdexcept>
#include <sys/types.h>
using Q = tuntom::ipc::RetryQueue;
static void require(bool ok, int line) { if (!ok) throw std::runtime_error("retry queue assertion at line " + std::to_string(line)); }
#define check(ok) require((ok), __LINE__)
int main() {
    auto now = Q::Clock::now();
    Q q;
    std::vector<std::uint8_t> data(100,7);
    auto blocked = [](const std::uint8_t*,std::size_t)->ssize_t { errno=EAGAIN; return -1; };
    auto r=q.submit(data.data(),data.size(),now,blocked);
    check(!r.frames && !r.drops && !q.empty() && !q.writable(now));
    data[0]=9;
    check(!q.enqueue(data.data(),data.size(),nullptr,0,now).drops);
    unsigned calls=0;
    auto send=[&](const std::uint8_t* p,std::size_t n)->ssize_t { check(p[0]==(calls++ ? 9:7)); return static_cast<ssize_t>(n); };
    check(q.flush(now,send).frames==0 && calls==0);
    r=q.flush(now+std::chrono::milliseconds(2),send);
    check(r.frames==2 && r.bytes==200 && q.empty());
    for (unsigned round=0;round<3;++round) {
        for(unsigned i=0;i<Q::capacity;++i) { data[0]=static_cast<uint8_t>(i); check(!q.enqueue(data.data(),100,nullptr,0,now).drops); }
        check(q.enqueue(data.data(),100,nullptr,0,now).backpressure==1);
        unsigned expected=0;
        r=q.flush(now,[&](const uint8_t* p,size_t n)->ssize_t { check(p[0]==expected++); return static_cast<ssize_t>(n); });
        check(r.frames==Q::capacity && q.empty());
    }
    std::vector<uint8_t> big(Q::record_limit,1);
    check(!q.enqueue(big.data(),big.size(),nullptr,0,now).drops);
    check(!q.enqueue(big.data(),big.size(),nullptr,0,now).drops);
    check(q.enqueue(data.data(),1,nullptr,0,now).drops==1);
    check(q.expire(now+Q::max_age).drops==2 && q.empty());
    now = Q::Clock::now();
    q.enqueue(data.data(),100,nullptr,0,now);
    r=q.flush(now,[](const uint8_t*,size_t)->ssize_t {errno=EINTR;return -1;});
    check(!r.drops && !r.error && !q.writable(now));
    r=q.flush(Q::Clock::now()+std::chrono::milliseconds(2),[](const uint8_t*,size_t)->ssize_t {errno=EPIPE;return -1;});
    check(r.error==EPIPE && r.drops==1 && q.empty());
    q.enqueue(data.data(),100,nullptr,0,now);
    check(q.discard().drops==1 && q.empty());
    std::cout << "PASS: bounded IPC FIFO, owned copies, wrap, byte limit, retry gate, expiry, errors and discard\n";
}
