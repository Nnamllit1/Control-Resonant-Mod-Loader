#include "runtime.h"
#include "mod_settings.h"
#include "mod_storage.h"
#include "mod_feedback.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

// Offline acceptance driver: real Wasm guests, typed settings and on-disk
// persistence. No gameplay provider, injected keyboard, renderer or game.
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=3) throw std::runtime_error("Expected mods and storage directories");
        crml::ModSettings settings;
        crml::ModStorage storage(argv[2]);
        uint64_t now{};
        crml::ModFeedback feedback([&]{return now;});
        feedback.enable_renderer(true);
        const auto page=feedback.open_page();
        uint64_t sequence{};
        if(!storage.available()) throw std::runtime_error("Storage unavailable");
        crml::Runtime runtime([](const std::string& message){std::cout<<message<<'\n';},nullptr,nullptr,
            [](std::string_view id,int,std::string_view text){std::cout<<id<<": "<<text<<'\n';},&storage,&settings,&feedback,[&]{return now;});
        runtime.load(argv[1]);
        const auto setting=[&](const std::string& id,const std::string& key) {
            for(const auto& group:settings.snapshot()) if(group.id==id)
                for(const auto& item:group.items) if(key==item.definition.key)
                    return std::pair{group.owner,item.state};
            throw std::runtime_error("Setting not found: "+id+"/"+key);
        };
        for(std::string line;std::getline(std::cin,line);) {
            std::istringstream input(line);std::string op,id,key;double value{};
            input>>op;
            if(op=="expect" || op=="set") {
                if(!(input>>id>>key>>value) || !std::isfinite(value)) throw std::runtime_error("Invalid setting command");
                const auto [owner,state]=setting(id,key);
                if(op=="set") {
                    if(settings.set(owner,state.handle,value,state.revision)<0) throw std::runtime_error("Setting rejected");
                } else if(state.value!=value) throw std::runtime_error("Unexpected preference: "+id+"/"+key);
            } else if(op=="tick") {
                unsigned ms{};if(!(input>>ms) || ms>60000) throw std::runtime_error("Invalid tick");
                now+=ms;
                runtime.tick(ms/1000.f);
            } else if(op=="clock") {
                if(!(input>>now)) throw std::runtime_error("Invalid clock");
            } else if(op=="storage_status") {
                int expected{};
                if(!(input>>id>>expected) || storage.status(id)!=expected) throw std::runtime_error("Unexpected storage state");
            } else if(op=="feedback" || op=="no_feedback") {
                std::string expected;
                if(!(input>>id) || (op=="feedback" && !(input>>std::quoted(expected)))) throw std::runtime_error("Invalid feedback assertion");
                std::vector<crml::ModFeedback::Message> messages;
                if(feedback.poll(page,++sequence,{},messages)!=200) throw std::runtime_error("Feedback poll failed");
                bool found=false;
                for(const auto& message:messages) if(message.id==id) {
                    if(op=="no_feedback" || message.text!=expected || !message.remaining_ms) throw std::runtime_error("Unexpected feedback");
                    found=true;
                }
                if(op=="feedback" && !found) throw std::runtime_error("Missing feedback");
            } else if(op=="settle") {
                int expected{};
                if(!(input>>id>>expected) || (expected!=2 && expected!=-5)) throw std::runtime_error("Invalid settle");
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(storage.status(id)==1 && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if(storage.status(id)!=expected) throw std::runtime_error("Unexpected storage completion");
                runtime.tick(0); // Let the guest observe completion.
            } else throw std::runtime_error("Unknown acceptance command");
            std::string extra;if(input>>extra) throw std::runtime_error("Unexpected extra arguments");
            if(runtime.failures()) throw std::runtime_error("Guest failure");
        }
        runtime.shutdown();
        if(runtime.failures()) throw std::runtime_error("Guest failure on shutdown");
        std::cout<<"Author services passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
