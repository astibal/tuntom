#pragma once
#include "control_route.hpp"
#include <functional>
#include <sstream>

namespace tuntom {
// Render discovery traces, retaining exact registration names for CLI targeting.
inline std::string discovery_tree(const std::string& snapshot, const control_route::Path& prefix = {}) {
    struct Node { std::map<std::string, Node> children; std::string label, state, component; };
    Node root; root.label = "switch";
    const auto descend = [](Node* node, const control_route::Hop& hop) {
        const bool peer = hop.type == control_route::HopType::peer;
        if (!peer && hop.type != control_route::HopType::port) throw std::runtime_error("unsupported discovery tree hop");
        auto& child = node->children[(peer ? "peer" : "port:" + hop.value)];
        child.label = peer ? "peer" : hop.value;
        return &child;
    };
    Node* base = &root;
    for (const auto& hop : prefix) base = descend(base, hop);
    std::istringstream input(snapshot); std::string line;
    if (!std::getline(input,line) || line != "path\tstate\tinstance\tcomponent\tcapabilities")
        throw std::runtime_error("invalid discovery header");
    const auto unescape = [](const std::string& text) {
        std::string name;
        const auto hex = [](char c) -> int {
            if(c>='0' && c<='9')return c-'0';
            if(c>='a' && c<='f')return c-'a'+10;
            if(c>='A' && c<='F')return c-'A'+10;
            return -1;
        };
        for(std::size_t i=0;i<text.size();++i) {
            unsigned char c=text[i];
            if(c=='%') {
                if(i+2>=text.size() || hex(text[i+1])<0 || hex(text[i+2])<0) throw std::runtime_error("invalid discovery escape");
                c=static_cast<unsigned char>((hex(text[i+1])<<4)|hex(text[i+2])); i+=2;
            }
            if(c<0x21 || c>0x7e)throw std::runtime_error("invalid discovery port name");
            name+=static_cast<char>(c);
        }
        if(name.empty() || name.size()>63)throw std::runtime_error("invalid discovery port name");
        return name;
    };
    std::size_t rows=0;
    while(std::getline(input,line)) {
        if(++rows>1024)throw std::runtime_error("too many discovery rows");
        std::array<std::string,5> fields;
        std::size_t at=0;
        for(std::size_t i=0;i<fields.size();++i) {
            const auto end=line.find('\t',at);
            if((i<4 && end==std::string::npos) || (i==4 && end!=std::string::npos))throw std::runtime_error("invalid discovery row");
            fields[i]=line.substr(at,end==std::string::npos?end:end-at); at=end+1;
        }
        if(fields[1]!="FOUND" && fields[1]!="ALT_PATH" && fields[1]!="NO_RESPONSE")throw std::runtime_error("invalid discovery state");
        if(fields[3].empty() || !std::all_of(fields[3].begin(),fields[3].end(),[](unsigned char c){return c>=0x21 && c<=0x7e;}))
            throw std::runtime_error("invalid discovery component");
        Node* node=base;
        // NO_RESPONSE describes the requested target, not an additional peer hop.
        if(fields[1]!="NO_RESPONSE" && fields[0]!="self") {
            std::istringstream path(fields[0]); std::string hop; unsigned depth=0;
            if(fields[0].empty() || fields[0].back()=='/')throw std::runtime_error("invalid discovery path");
            while(std::getline(path,hop,'/')) {
                if(++depth>control_route::max_hops)throw std::runtime_error("discovery path too deep");
                if(hop=="peer")node=descend(node,control_route::Hop::peer());
                else if(hop.compare(0,5,"port:")==0)node=descend(node,control_route::Hop::port(unescape(hop.substr(5))));
                else throw std::runtime_error("invalid discovery hop");
            }
        }
        if(node->state.empty() || fields[1]=="FOUND") {node->state=fields[1];node->component=fields[3];}
    }
    std::string out;
    const auto label = [](const Node& node) {
        std::string value=node.label;
        if(!node.component.empty() && node.component!="-" && node.component!=node.label)value+=" ["+node.component+"]";
        if(node.state=="ALT_PATH" || node.state=="NO_RESPONSE")value+=" ["+node.state+"]";
        return value;
    };
    out=label(root)+"\n";
    std::function<void(const Node&,const std::string&)> render;
    render=[&](const Node& node,const std::string& indent) {
        std::size_t i=0;
        for(const auto& item:node.children) {
            const bool last=++i==node.children.size();
            out+=indent+(last?"`-- ":"|-- ")+label(item.second)+"\n";
            if(out.size()>control_max_body)throw std::runtime_error("discovery tree exceeds response limit");
            render(item.second,indent+(last?"    ":"|   "));
        }
    };
    render(root,"");
    if(root.children.empty() && root.state!="NO_RESPONSE")out+="`-- (no discovered ports)\n";
    return out;
}
} // namespace tuntom
