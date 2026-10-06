#include "../src/routed_control.hpp"
#include <stdexcept>
#include <iostream>
namespace tuntom {
struct RoutedControlLifecycleTest {
    static void require(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
    static void run() {
        RoutedControl control("test");
        auto now=RoutedControl::Clock::now();
        RoutedControl::Id origin{};origin[0]=1;
        auto receiver=std::make_unique<RoutedControl::Receiver>();
        receiver->used=now;
        int executions=0;
        receiver->transactions.configure(ControlAccess::all(),[](const auto&) {},[&](const auto&) {
            ++executions;return ControlResponse{true,"saved\n"};
        });
        remote_control::Frame request;
        request.id[0]=2;request.kind=remote_control::Kind::put;
        request.command="classifier disable 0";
        receiver->transactions.receive(remote_control::encode(request),now);
        require(executions==1,"fixture transaction executed");
        receiver->replies[request.id]={};receiver->reply_seen[request.id]=now;
        auto orphan=request.id;orphan[0]=3;
        receiver->replies[orphan]={};receiver->reply_seen[orphan]=now;
        auto* retained=receiver.get();
        control.receivers_.emplace(origin,std::move(receiver));
        control.tick(now+std::chrono::minutes(6));
        require(!retained->replies.count(orphan),"orphan route expires independently");
        require(retained->replies.count(request.id),"unconfirmed receipt keeps its route");
        require(retained->transactions.received_state(request.id).has_value(),"execution receipt retained");
        // Acknowledged routing metadata may expire; its execution receipt remains.
        retained->confirmed.insert(request.id);
        control.tick(now+std::chrono::minutes(7));
        require(!retained->replies.count(request.id),"confirmed route expires");
        require(retained->transactions.received_state(request.id).has_value(),"route cleanup never erases transaction history");
        retained->transactions.receive(remote_control::encode(request),now+std::chrono::minutes(7));
        require(executions==1,"repeated request does not execute again");
        std::ostringstream stats;control.write_stats(stats);
        require(stats.str().find("control_routed_pruned_routes=2\n")!=std::string::npos,"cleanup diagnostic");
        require(stats.str().find("control_routed_busy_reply_route_limit=0\n")!=std::string::npos,"capacity diagnostic");
    }
};
}
int main() {tuntom::RoutedControlLifecycleTest::run();std::cout<<"routed lifecycle OK\n";}
