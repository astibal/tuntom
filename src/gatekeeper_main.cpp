#include "version.hpp"
#include "gatekeeper.hpp"
#include <iostream>

int main(int argc,char** argv) {
    if (tuntom::print_version_if_requested(argc, argv, "tuntom-gatekeeper")) return 0;
    try {
        if(argc!=3||std::string(argv[1])!="verify")throw std::runtime_error("usage: tuntom-gatekeeper verify <config>");
        tuntom::auth_helper::Bytes input((std::istreambuf_iterator<char>(std::cin)),{});tuntom::auth_helper::Verify request;
        if(!tuntom::auth_helper::decode(input,request))return 2;
        const auto output=tuntom::auth_helper::encode(tuntom::gatekeeper::verify(tuntom::gatekeeper::load(argv[2]),request));
        std::cout.write(reinterpret_cast<const char*>(output.data()),static_cast<std::streamsize>(output.size()));return output.empty()?2:0;
    } catch(const std::exception& e) { std::cerr<<"tuntom-gatekeeper: "<<e.what()<<'\n';return 2; }
}
