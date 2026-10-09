#include "mod_storage.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs=std::filesystem;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int wait(crml::ModStorage& store,std::string_view id) {
    for(int i=0;i<500;++i) {const auto result=store.status(id);if(result!=1)return result;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    throw std::runtime_error("storage worker did not finish");
}
int main() {
    const auto base=fs::current_path()/("storage-test-"+std::to_string(GetCurrentProcessId()));
    try {
        require(fs::create_directory(base),"unique test directory");
        const auto root=base/"data";
        std::array<unsigned char,4> bytes{1,0,2,255},out{9,9,9,9};
        {
            crml::ModStorage store(root);require(store.available(),"available");
            crml::ModStorage competing(root);require(!competing.available(),"exclusive directory ownership");
            store.attach("alpha");store.attach("beta");store.attach("../escape");
            require(store.status("../escape")==-1,"invalid IDs unavailable");
            require(store.status("alpha")==0 && store.read("alpha",out)==-2,"absent record");
            require(out[0]==9,"failure preserves output");
            std::vector<unsigned char> large(65537);
            require(store.write("alpha",large)==-3,"oversized payload");
            require(store.write("alpha",bytes)==0,"queue write");
            bytes[0]=8; // accepted bytes must have been copied
            require(store.write("alpha",bytes)==-4,"pending or rate-limited replacement");
            require(wait(store,"alpha")==2,"committed status");
            require(store.status("alpha")==2,"status is retained");
            require(store.read("alpha",std::span(out).first(2))==-3 && out[0]==9,"short buffer does not modify output");
            require(store.read("alpha",out)==4 && out[0]==1 && out[3]==255,"copied binary record");
            require(store.read("beta",out)==-2,"IDs are isolated");
            require(store.write("beta",{})==0 && wait(store,"beta")==2,"empty record commit");
            require(store.read("beta",{})==0,"empty is distinct from absent");
            for(int i=0;i<30;++i)store.attach("limit-"+std::to_string(i));
            store.attach("overflow");require(store.status("overflow")==-1,"registration bound");
        }
        {
            crml::ModStorage store(root);store.attach("alpha");store.attach("beta");
            require(store.read("alpha",out)==4 && out[0]==1,"restart restores committed data");
            require(store.read("beta",out)==0,"empty survives restart");
            require(store.status("alpha")==0,"request state is session-local");
            // Simulate an interrupted previous process, then recover on save.
            std::ofstream(root/"mod-alpha.pending",std::ios::binary)<<"partial";
            require(store.write("alpha",bytes)==0 && wait(store,"alpha")==2,"abandoned staging recovered");
            require(!fs::exists(root/"mod-alpha.pending"),"staging removed on commit");
        }
        {
            crml::ModStorage store(root);store.attach("alpha");
            HANDLE held=CreateFileW((root/"mod-alpha.bin").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            require(held!=INVALID_HANDLE_VALUE,"hold old record to force replacement failure");
            bytes[0]=5;
            require(store.write("alpha",bytes)==0,"queue failed replacement");
            const int result=wait(store,"alpha");CloseHandle(held);
            require(result==-5,"replacement error visible");
            require(store.read("alpha",out)==4 && out[0]==8,"failed replacement preserves cache");
            require(!fs::exists(root/"mod-alpha.pending"),"failed replacement cleans staging");
        }
        {
            crml::ModStorage store(root);store.attach("alpha");
            require(store.read("alpha",out)==4 && out[0]==8,"failed replacement preserves disk across restart");
            std::vector<unsigned char> full(65536,42);
            require(store.write("alpha",full)==0,"queue maximum record before destructor");
            // Destruction must drain this accepted write.
        }
        {
            crml::ModStorage store(root);store.attach("alpha");
            std::vector<unsigned char> full(65536);
            require(store.read("alpha",full)==65536 && full.back()==42,"drain and maximum-size round trip");
        }
        // Invalid file, checksum corruption and hardlink reads must not escape.
        std::ofstream(root/"mod-bad.bin",std::ios::binary)<<"invalid";
        fs::copy_file(root/"mod-alpha.bin",root/"mod-corrupt.bin");
        {std::fstream file(root/"mod-corrupt.bin",std::ios::binary|std::ios::in|std::ios::out);file.seekp(20);file.put(0);}
        fs::create_hard_link(root/"mod-alpha.bin",root/"mod-linked.bin");
        {
            crml::ModStorage store(root);
            for(const auto id:{"bad","corrupt","linked"}) {store.attach(id);require(store.read(id,out)==-5,"reject corrupt and linked records");}
            // Replacing a hardlink entry must leave the linked target unchanged.
            require(store.write("linked",bytes)==0 && wait(store,"linked")==2,"safe entry replacement");
        }
        require(fs::file_size(root/"mod-alpha.bin")==65552,"linked target untouched");
        // A file at the directory root must disable persistence, not throw into
        // mod initialization or grant a storage capability.
        std::ofstream(base/"not-a-directory")<<"x";
        {crml::ModStorage unavailable(base/"not-a-directory");require(!unavailable.available(),"invalid root unavailable");}
        fs::remove_all(base);
        std::cout<<"Storage ownership, failure, bounds and restart checks passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';std::error_code ignored;fs::remove_all(base,ignored);return 1;
    }
}
