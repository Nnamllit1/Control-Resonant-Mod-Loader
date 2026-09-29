#include "lua_source.h"
#include "lua_packages.h"
#include <atomic>
#include <fstream>
#include <map>
#include <sstream>

namespace crml::engine::lua::source {
namespace {
struct Service {
    std::filesystem::path root;
    ControllerHost host;
    SourcePackages packages;
    std::ofstream log;
    size_t written{};
    uint64_t next_poll{},next_report{};
    std::atomic<bool> running{true};
    bool stopping{},logging{true};
    std::map<std::string,std::pair<std::string,unsigned>> previous_events;
    Service(std::filesystem::path path,Api api):root(std::move(path)),host(api),packages(root/"lua-mods",host),
        log(root/"lua-mods.jsonl",std::ios::trunc) {}
    void emit(const std::string& line) {
        if(!logging || !log) return;
        constexpr size_t limit=4*1024*1024;
        if(written+line.size()+1>limit-64) {
            log<<"{\"type\":\"log_limit\"}\n";log.flush();logging=false;return;
        }
        log<<line<<'\n';log.flush();written+=line.size()+1;
    }
    void report(uint64_t now) {
        const auto s=host.snapshot();
        std::ostringstream out;
        out<<"{\"type\":\"lua_source\",\"schema\":1,\"tick_ms\":"<<now
           <<",\"trusted\":true,\"halted\":"<<(s.halted?"true":"false")
           <<",\"packages\":"<<s.packages<<",\"references\":"<<s.references<<",\"pending\":"<<s.pending
           <<",\"failed\":"<<s.failed<<",\"awaiting_owner\":"<<s.awaiting_owner<<",\"initialized\":"<<s.initialized
           <<",\"invoked\":"<<s.invoked<<",\"released\":"<<s.released<<",\"reclaimed\":"<<s.reclaimed
           <<",\"failures\":"<<s.failures<<",\"rejected\":"<<s.rejected<<",\"interrupted\":"<<s.interrupted
           <<",\"ambiguous_releases\":"<<s.ambiguous_releases<<'}';
        emit(out.str());
        for(const auto& p:host.packages()) {
            std::ostringstream item;
            item<<"{\"type\":\"lua_package\",\"tick_ms\":"<<now<<",\"id\":\""<<p.id<<"\",\"state\":\""<<p.state
                <<"\",\"active_revision\":"<<p.active_revision<<",\"desired_revision\":"<<p.desired_revision
                <<",\"calls\":"<<p.calls<<",\"status\":"<<p.status<<",\"line\":"<<p.line<<'}';
            emit(item.str());
        }
        next_report=now+1000;
    }
};
std::atomic<bool> wanted{};
std::atomic<Service*> service{};
std::filesystem::path prepared_root;
bool opt_in(const std::filesystem::path& root,std::error_code& ec) {
    const auto status=std::filesystem::symlink_status(root/"engine-lua.enabled",ec);
    if(status.type()==std::filesystem::file_type::not_found) {ec.clear();return false;}
    return !ec && std::filesystem::is_regular_file(status);
}
}
bool prepare(const std::filesystem::path& root) {
    if(service.load() || wanted.load()) return false;
    std::error_code ec;
    if(!opt_in(root,ec) || ec) return false;
    prepared_root=root;wanted.store(true,std::memory_order_release);return true;
}
bool requested() noexcept {return wanted.load(std::memory_order_acquire);}
bool attach(Api api) noexcept {
    if(!requested() || service.load()) return false;
    try {
        auto candidate=std::make_unique<Service>(prepared_root,api);
        const auto status=candidate->host.snapshot();
        if(!candidate->log || !status.configured || status.halted) return false;
        candidate->emit("{\"type\":\"lua_source_start\",\"schema\":1,\"compiler\":\"Luau 0.650\",\"trusted\":true}");
        // Pinned for the process, like the detour code. A worker ending must not
        // destroy a host whose roots still await engine cleanup or VM closure.
        service.store(candidate.release(),std::memory_order_release);return true;
    } catch(...) {return false;}
}
bool active() noexcept {const auto* s=service.load(std::memory_order_acquire);return s && s->running.load();}
bool needs_calls() noexcept {return service.load(std::memory_order_acquire)!=nullptr;}
void tick(Context context,uint64_t now) {if(auto* s=service.load(std::memory_order_acquire)) s->host.tick(context,now);}
void stop() noexcept {
    if(auto* s=service.load(std::memory_order_acquire)) {s->stopping=true;s->host.stop();}
}
void poll(uint64_t now) {
    auto* s=service.load(std::memory_order_acquire);
    if(!s || !s->running.load() || now<s->next_poll) return;
    s->next_poll=now+500;
    try {
        if(!s->stopping) {
            std::error_code ec;
            const bool enabled=opt_in(s->root,ec);
            if(ec) s->emit("{\"type\":\"lua_package_event\",\"event\":\"opt_in_unreadable\"}");
            else {
                std::map<std::string,std::pair<std::string,unsigned>> current;
                for(const auto& event:s->packages.poll(enabled)) {
                    const auto key=std::make_pair(std::string(event.kind),event.line);
                    current[event.id]=key;
                    if(s->previous_events.contains(event.id) && s->previous_events.at(event.id)==key) continue;
                    std::ostringstream item;
                    item<<"{\"type\":\"lua_package_event\",\"tick_ms\":"<<now<<",\"id\":\""<<event.id
                        <<"\",\"event\":\""<<event.kind<<"\",\"line\":"<<event.line<<'}';
                    s->emit(item.str());s->next_report=0;
                }
                s->previous_events=std::move(current);
            }
        }
        if(now>=s->next_report) s->report(now);
        if(s->stopping && !s->host.snapshot().references) {
            s->report(now);s->emit("{\"type\":\"lua_source_stopped\"}");s->log.close();s->running=false;
        }
    } catch(...) {
        // Keep the pinned host available to drain. Never publish arbitrary
        // exception text or destroy retained callbacks from this worker.
        s->stopping=true;s->host.stop();
        s->emit("{\"type\":\"lua_source_error\",\"error\":\"worker_failure\"}");
    }
}
}
