#include "lua_packages.h"
#include "lua_probe_bytecode.h"
#include <Windows.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>

using namespace crml::engine::lua;
namespace fs=std::filesystem;
namespace {
std::map<int,std::vector<unsigned char>> roots;
std::vector<Action> calls;
int references{},compilations{};
fs::path changing_source;
const std::string controller="local count = 0\nreturn function(stop)\n if stop then return end\n count += 1\nend\n";
void require(bool ok,const char* text) {if(!ok) {std::cerr<<text<<'\n';std::exit(1);}}
void write(const fs::path& path,const std::string& text) {fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file<<text;require(bool(file),"write fixture");}
std::string read(const fs::path& path) {std::ifstream file(path,std::ios::binary);require(bool(file),"open fixture source");return {std::istreambuf_iterator<char>(file),{}};}
Compilation compiler(const std::string& source) {
    ++compilations;
    auto result=compile_source(source);
    if(!changing_source.empty()) {write(changing_source,controller+"-- saved during compile\n");changing_source.clear();}
    return result;
}
Execution provider(const Api&,Context,Action action,int ref,Program program,Options,Liveness live) {
    require(live.owner_live() && live.vm_live(),"package executes under live host gate");
    calls.push_back(action);Execution result;result.attempted=result.restored=true;
    if(action==Action::initialize) {result.reference=++references;roots[references]={program.bytes.begin(),program.bytes.end()};}
    else {
        require(roots.contains(ref),"package callback has retained root");
        require(std::equal(program.bytes.begin(),program.bytes.end(),roots.at(ref).begin(),roots.at(ref).end()),"retained callback keeps its original compiled revision");
        if(action==Action::unload || action==Action::release) {roots.erase(ref);result.release_attempted=result.released=true;result.shutdown=action==Action::unload;}
    }
    return result;
}
void tick(ControllerHost& host,uint64_t time) {host.tick({reinterpret_cast<void*>(1),10,20,30,dispatch::revision()},time);}
bool event(const std::vector<PackageEvent>& events,const char* kind,const std::string& id="sample") {
    for(const auto& e:events) if(e.id==id && std::string_view(e.kind)==kind) return true;
    return false;
}
}
int main(int argc,char** argv) {
    require(argc==2,"repository source directory required");
    namespace bc=crml::probe::lua::bytecode;
    struct Example {const char* name;std::span<const unsigned char> bytes;};
    const Example examples[]{{"arithmetic",bc::arithmetic},{"error",bc::error},{"bindings",bc::bindings},
        {"events",bc::events},{"persistent",bc::persistent},{"persistent_error",bc::persistent_error},{"persistent_events",bc::persistent_events}};
    for(const auto& example:examples) {
        const auto compiled=compile_source(read(fs::path(argv[1])/"examples/lua-probe"/(std::string(example.name)+".luau")));
        require(bool(compiled) && std::equal(compiled.bytes.begin(),compiled.bytes.end(),example.bytes.begin(),example.bytes.end()),"linked compiler reproduces gameplay-tested bytecode exactly");
    }
    const auto bad=compile_source("local = private_identifier\n");
    require(!bad && std::string_view(bad.error)=="compile_error" && bad.line==1,"syntax error reports category and line without source text");
    require(std::string_view(compile_source(std::string("\xff")).error)=="invalid_utf8","invalid UTF8 rejected before compilation");
    require(compile_source("\xef\xbb\xbf"+controller).bytes==compile_source(controller).bytes,"UTF8 BOM does not change compiler output");
    require(std::string_view(compile_source(std::string(max_source_bytes+1,' ')).error)=="source_too_large","source size bounded");

    const auto temporary=fs::absolute("lua-source-test-"+std::to_string(GetCurrentProcessId()));
    require(fs::create_directory(temporary),"create exclusively owned fixture directory");
    struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{temporary};
    const auto root=temporary/"lua-mods",entry=root/"sample/main.luau";
    ControllerHost host({},&provider);SourcePackages packages(root,host,&compiler);
    require(packages.poll().empty() && calls.empty(),"missing package directory is passive");
    write(entry,controller);
    auto events=packages.poll();require(event(events,"revision_queued") && calls.empty() && compilations==1,"worker compiles source without invoking VM");
    tick(host,0);tick(host,16);require(roots.size()==1 && host.snapshot().invoked==1,"compiled source submitted to real lifecycle host");
    require(packages.poll().empty() && compilations==1,"unchanged source not recompiled");
    write(entry,"local = private_source_text\n");events=packages.poll();
    require(event(events,"compile_error") && events.front().line==1 && roots.size()==1,"invalid edit keeps last working controller");
    require(packages.poll().empty() && compilations==2,"unchanged invalid source not retried");
    write(entry,controller+"-- replacement\n");require(event(packages.poll(),"revision_queued"),"valid edit queues replacement");
    tick(host,17);require(calls.back()==Action::unload && roots.empty(),"source edit drains old controller");
    tick(host,18);require(calls.back()==Action::initialize && roots.size()==1,"source edit initializes new controller");
    write(root/"sample/disabled","");require(event(packages.poll(),"unload_queued"),"disabled marker requests unload");
    tick(host,19);tick(host,20);require(roots.empty(),"disabled source drained on engine context");
    fs::remove(root/"sample/disabled");require(event(packages.poll(),"revision_queued"),"removing disabled marker recompiles source");tick(host,21);
    write(entry,controller+"-- editor first save\n");changing_source=entry;
    require(event(packages.poll(),"source_changed") && roots.size()==1,"source changed during compilation never replaces active revision");
    require(event(packages.poll(),"revision_queued"),"stable next snapshot can load");tick(host,22);tick(host,23);
    fs::remove(entry);require(event(packages.poll(),"unload_queued"),"removed source requests unload");tick(host,24);tick(host,25);
    write(root/"raw/main.luac",std::string(reinterpret_cast<const char*>(bc::arithmetic),sizeof(bc::arithmetic)));
    require(packages.poll().empty() && roots.empty(),"package discovery never executes raw bytecode files");
    write(entry,std::string(reinterpret_cast<const char*>(bc::arithmetic),sizeof(bc::arithmetic)));
    require(!event(packages.poll(),"revision_queued") && roots.empty(),"raw bytecode renamed as source is not accepted as compiler output");
    write(entry,std::string(max_source_bytes+1,' '));require(event(packages.poll(),"source_too_large"),"oversized source file rejected before read");
    write(entry,controller);packages.poll();tick(host,26);
    const auto moved=temporary/"moved";fs::rename(root,moved);write(root,"not a directory");
    require(event(packages.poll(),"package_root_unavailable","") && roots.size()==1,"uncertain root scan preserves accepted revision");
    require(packages.poll().empty(),"repeated root errors bounded");fs::remove(root);fs::rename(moved,root);
    for(unsigned i=0;i<17;++i) fs::create_directories(root/("p"+std::to_string(i)));
    require(event(packages.poll(),"package_count_limit","") && roots.size()==1,"oversized directory set does not partially unload accepted packages");
    require(event(packages.poll(false),"unload_queued"),"global opt-out drains even when discovery is unavailable");
    tick(host,27);tick(host,28);require(roots.empty() && host.snapshot().packages==0,"all accepted source packages drained");
    std::cout<<"Linked Luau compiler and source package checks passed\n";
}
