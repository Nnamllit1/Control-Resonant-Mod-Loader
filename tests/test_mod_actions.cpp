#include "mod_actions.h"
#include "action_bindings.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
    try {
        crml::ModActions actions;
        crml::ModActions::Keys first{};first[0]=0x79;first[15]=0x79;
        require(!actions.attach(0,first),"zero owner");
        auto invalid=first;invalid[2]=256;
        require(!actions.attach(1,invalid),"invalid native code");
        require(actions.attach(1,first) && !actions.attach(1,first),"attach owner exactly once");
        crml_input_state state{};
        require(actions.read(1,state)==1 && state.size==232 && state.version==1 && state.binding_revision==1,
                "snapshot ABI and initial revision");
        require(state.bound==0x8001 && state.duplicate==0x8001 && !state.shared && !state.host_shortcut &&
                !state.held && !state.flags && !std::strcmp(state.names[15],"F10") && !std::strcmp(state.names[1],"None"),
                "local aliases are not other owners");
        crml::ModActions::Keys second{};second[2]=0x79;
        require(actions.attach(2,second) && actions.read(1,state)==1 && state.shared==0x8001,"other mod shares key");
        require(actions.bind(2,2,"F11")==1 && actions.read(2,state)==1 && state.host_shortcut==4 && !state.shared,
                "rebind moves shared count and reports native shortcut");
        require(actions.read(1,state)==1 && !state.shared && state.binding_revision==1,"peer changes do not change local revision");
        require(actions.bind(1,0,"F10")==0 && actions.bind(1,16,"F10")==-3 &&
                actions.bind(1,0,"Escape")==-3 && actions.bind(1,0,"0x1b")==-3 &&
                actions.bind(1,0,std::string_view("W\0",2))==-3,"no-op and invalid names");
        require(actions.bind(1,0,"None")==1 && actions.read(1,state)==1 && state.bound==0x8000 && !state.duplicate,
                "remove only one local alias");
        require(actions.bind(2,2,"F10")==1 && actions.read(1,state)==1 && state.shared==0x8000,"remaining alias retains owner count");
        actions.detach(1);actions.detach(1);
        require(actions.read(2,state)==1 && !state.shared,"detach idempotently removes only one owner");
        require(actions.read(1,state)==-1 && state.size==0 && state.names[0][0]==0 && !actions.keys(1),"detached owner zero snapshot");
        require(actions.bind(1,0,"W")==-1,"cannot rebind detached owner");
        for(const auto& key:crml::action_keys) {
            require(key.name.size()<CRML_INPUT_KEY_NAME_CAPACITY && crml::action_name(key.code)==key.name,"key round trip");
            require(actions.bind(2,15,key.name)>=0 && actions.read(2,state)==1 &&
                    std::string_view(state.names[15])==key.name,"every key fits copied ABI");
        }
        for(uint64_t id=3;id<=33;++id)require(actions.attach(id,second),"all 32 owners fit");
        require(!actions.attach(34,second),"owner capacity bounded");
        actions.detach(20);require(actions.attach(34,second),"retired slot can be reused");
        for(uint64_t id=2;id<=34;++id)actions.detach(id);
        require(actions.attach(90,first) && actions.read(90,state)==1 && !state.shared,"no stale shared counts after all cleanup");
        std::cout<<"Bounded action bindings, copied names, native/peer conflicts and owner cleanup passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
