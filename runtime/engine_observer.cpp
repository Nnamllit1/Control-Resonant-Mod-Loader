#include "engine_observer.h"
#include "physics_observation.h"
#include <MinHook.h>
#include <bcrypt.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace crml::observer {
void Buffer::open() noexcept {
    AcquireSRWLockExclusive(&lock_);
    head_=size_=sequence_=0;
    for(auto& count:counts_) count.store(0);
    dropped_.store(0); accepting_=true;
    ReleaseSRWLockExclusive(&lock_);
}
void Buffer::close() noexcept {
    AcquireSRWLockExclusive(&lock_); accepting_=false; ReleaseSRWLockExclusive(&lock_);
}
bool Buffer::push(Event event) noexcept {
    if(static_cast<size_t>(event.kind)>=counts_.size()) return false;
    counts_[static_cast<size_t>(event.kind)].fetch_add(1,std::memory_order_relaxed);
    if(!TryAcquireSRWLockExclusive(&lock_)) { dropped_.fetch_add(1); return false; }
    const bool room=accepting_ && size_<events_.size();
    if(room) { event.sequence=++sequence_; events_[(head_+size_)%events_.size()]=event; ++size_; }
    else if(accepting_) dropped_.fetch_add(1);
    ReleaseSRWLockExclusive(&lock_);
    return room;
}
size_t Buffer::drain(Event* output,size_t limit) noexcept {
    AcquireSRWLockExclusive(&lock_);
    const auto count=(std::min)(limit,size_);
    for(size_t i=0;i<count;++i) output[i]=events_[(head_+i)%events_.size()];
    head_=(head_+count)%events_.size(); size_-=count;
    ReleaseSRWLockExclusive(&lock_); return count;
}
uint64_t identity(uintptr_t pointer,uint64_t salt) noexcept {
    if(!pointer) return 0;
    uint64_t value=pointer^salt;
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ull;
    value=(value^(value>>27))*0x94d049bb133111ebull;
    value^=value>>31;
    return value?value:1;
}
ResourceSample read_resource(uintptr_t pointer) noexcept {
    ResourceSample result{};
    if(!pointer) return result;
    __try {
        result.id=*reinterpret_cast<const uint64_t*>(pointer+8);
        result.refs=*reinterpret_cast<const uint32_t*>(pointer+0x64);
        result.state=*reinterpret_cast<const uint32_t*>(pointer+0x68);
        result.pointer=pointer; result.readable=true;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { result={}; }
    return result;
}
void write_event(std::ostream& out,const Event& e) {
    out << "{\"type\":\"event\",\"sequence\":" << e.sequence << ",\"qpc\":" << e.qpc
        << ",\"thread\":" << e.thread << ",\"kind\":\"" << names[static_cast<size_t>(e.kind)]
        << "\",\"edge\":" << unsigned(e.edge) << ",\"span\":" << e.span
        << ",\"object\":\"" << e.object << "\",\"entity\":\"" << e.entity
        << "\",\"value\":\"" << e.value << "\",\"detail\":\"" << e.detail << "\",\"flags\":" << e.flags << '}';
}
namespace {
constexpr char fingerprint_expected[]="2c6575be23ea9a2d316fb530d094773b371ab1da6344aa7a97b8cc2dabaf1ca0";
constexpr char physx_expected[]="ec53e67b700e67a5420a2c951340efd9a8ed9d086da539eed6ee88368fe0ce51";
constexpr uint64_t max_bytes=64*1024*1024, max_milliseconds=10*60*1000;
Buffer buffer;
std::atomic<bool> enabled{};
std::atomic<uint64_t> spans{}, next_player_ms{};
std::atomic<uint64_t> next_body_ms{},body_cursor{};
std::atomic<uint64_t> entity_cursor{};
std::atomic<uint64_t> movement_calls{}, player_calls{};
uintptr_t image_base{};
uintptr_t dynamic_vtable{};
uint64_t salt{};
using One=void(*)(void*);
using Three=void(*)(void*,void*,void*);
using Move=void(*)(void*,void*,void*,void*,void*,void*);
using Simulate=void(*)(void*,float);
Move move_original{};
One flush_original{},renderer_original{},wait_original{},complete_original{},post_original{};
Three script_original{};
Simulate begin_original{};

uintptr_t pointer_at(uintptr_t address) noexcept {
    __try { return address?*reinterpret_cast<const uintptr_t*>(address):0; }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return 0; }
}
Event event(Kind kind,uint8_t edge,uint64_t span,uintptr_t object) noexcept {
    LARGE_INTEGER time{}; QueryPerformanceCounter(&time);
    Event e{}; e.qpc=time.QuadPart; e.thread=GetCurrentThreadId(); e.kind=kind;
    e.edge=edge; e.span=span; e.object=identity(object,salt); return e;
}
uint64_t enter(Kind kind,uintptr_t object) noexcept {
    if(!enabled.load(std::memory_order_relaxed)) return 0;
    const auto span=spans.fetch_add(1,std::memory_order_relaxed)+1;
    buffer.push(event(kind,1,span,object)); return span;
}
void leave(Kind kind,uint64_t span,uintptr_t object) noexcept {
    if(span) buffer.push(event(kind,2,span,object));
}
bool player_sample(void* view,void* world,probe::Sample& sample) noexcept {
    __try {
        const auto tag=*reinterpret_cast<const uint16_t*>(image_base+0x5c00ca4);
        return probe::inspect(view,world,tag,sample)==probe::Observation::player;
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return false; }
}
void sample_resources(const probe::Sample& player) noexcept {
    struct Component { uint32_t hash,stride; };
    constexpr Component components[]={{0xeece657a,8},{0xe1da5fb1,16},{0x6ebfd07c,16}};
    for(const auto component:components) {
        uintptr_t chunk{}; uint32_t row{};
        const auto at=probe::entity_component(player.world,player.entity,component.hash,component.stride,chunk,row);
        auto e=event(Kind::resource,0,0,player.world); e.entity=player.entity; e.value=component.hash;
        if(at) {
            e.flags=1; // component observed; pointer read failure still leaves resource unreadable
            const auto pointer=pointer_at(at);
            const auto resource=read_resource(pointer);
            if(resource.readable) {
                e.flags|=2; e.object=identity(pointer,salt); e.span=identity(player.world,salt);
                e.detail=resource.id; // Values copied before returning from owning callback.
                // Extra observation records keep the fixed producer record bounded.
                auto state=e; state.flags|=4; state.detail=(uint64_t(resource.refs)<<32)|resource.state;
                buffer.push(e); buffer.push(state); continue;
            }
        }
        e.span=identity(player.world,salt); e.object=0; buffer.push(e);
    }
}
void sample_entity_bodies(uintptr_t world,uint64_t movement_span) noexcept {
    const auto owner=world_scene(world);
    const auto count=body_slot_count(owner);
    auto scan=event(Kind::entity_scan,0,identity(world,salt),owner);
    auto access_scan=event(Kind::accessor_scan,0,movement_span,owner);
    const auto physx_base=dynamic_vtable-0x15dd88;
    const DampingAccessors accessors{dynamic_vtable,reinterpret_cast<DampingGetter>(physx_base+0x22360),
        reinterpret_cast<DampingGetter>(physx_base+0x21870)};
    scan.detail=count;
    if(!owner) scan.flags=1u<<(16+static_cast<unsigned>(LinkRead::world));
    if(count) {
        const auto start=entity_cursor.load(std::memory_order_relaxed)%count;
        for(uint32_t i=0;i<(std::min)(count,64u) && scan.entity<4;++i) {
            BodySnapshot body{};
            const auto result=read_body(owner,static_cast<uint32_t>((start+i)%count),dynamic_vtable,body);
            ++scan.value;
            if(result!=BodyRead::ok) {scan.flags|=1u<<static_cast<unsigned>(result);continue;}
            BodyEntitySnapshot link{};
            const auto linked=read_body_entity(world,owner,body,link);
            if(linked!=LinkRead::ok) {scan.flags|=1u<<(16+static_cast<unsigned>(linked));continue;}
            auto e=event(Kind::entity_body,0,scan.span,owner);
            e.entity=link.entity; e.value=body.handle; e.detail=identity(body.actor,salt); e.flags=1;
            buffer.push(e); ++scan.entity;
            DampingReadback values{}; ++access_scan.value;
            const auto accessed=read_damping_accessors(owner,body,accessors,values);
            if(accessed!=AccessRead::ok) access_scan.flags|=1u<<static_cast<unsigned>(accessed);
            if(accessed==AccessRead::ok || accessed==AccessRead::mismatch) {
                auto a=event(Kind::body_accessor,0,movement_span,body.actor);
                a.entity=body.handle; a.detail=identity(owner,salt);
                a.value=uint64_t(std::bit_cast<uint32_t>(values.linear))|(uint64_t(std::bit_cast<uint32_t>(values.angular))<<32);
                a.flags=(accessed==AccessRead::ok?1u:4u)|(values.alternate?2u:0u);
                buffer.push(a); ++access_scan.entity;
            }
        }
        entity_cursor.store((start+scan.value)%count,std::memory_order_relaxed);
    }
    buffer.push(scan);
    buffer.push(access_scan);
}
void movement(void* view,void* world,void* collision,void* callback,void* scene,void* time) {
    uint64_t span{}; probe::Sample sample{};
    if(enabled.load(std::memory_order_relaxed)) {
        movement_calls.fetch_add(1,std::memory_order_relaxed);
        if(player_sample(view,world,sample)) {
            player_calls.fetch_add(1,std::memory_order_relaxed);
            span=enter(Kind::movement,sample.world);
            const auto now=GetTickCount64();
            auto next=next_player_ms.load(std::memory_order_relaxed);
            if(now>=next && next_player_ms.compare_exchange_strong(next,now+100)) {
                auto e=event(Kind::player,0,span,sample.world); e.entity=sample.entity; e.value=sample.row;
                e.flags=1; e.detail=uint64_t(sample.disabled)|(uint64_t(sample.teleported)<<8);
                buffer.push(e); sample_resources(sample); sample_entity_bodies(sample.world,span);
            }
        }
    }
    move_original(view,world,collision,callback,scene,time);
    leave(Kind::movement,span,sample.world);
}
void flush(void* view) {
    const auto world=enabled.load()?pointer_at(reinterpret_cast<uintptr_t>(view)):0;
    const auto span=enter(Kind::command_flush,world); flush_original(view); leave(Kind::command_flush,span,world);
}
void script(void* world,void* query,void* environment) {
    const auto pointer=enabled.load()?pointer_at(reinterpret_cast<uintptr_t>(world)):0;
    const auto span=enter(Kind::script_fixed,pointer); script_original(world,query,environment); leave(Kind::script_fixed,span,pointer);
}
void renderer(void* argument) {
    const auto span=enter(Kind::renderer_sync,0); renderer_original(argument); leave(Kind::renderer_sync,span,0);
}
void begin(void* wrapper,float seconds) {
    const auto object=reinterpret_cast<uintptr_t>(wrapper);
    const auto span=enter(Kind::physics_begin,object); begin_original(wrapper,seconds); leave(Kind::physics_begin,span,object);
}
void wait(void* owner) {
    const auto wrapper=enabled.load()?pointer_at(reinterpret_cast<uintptr_t>(owner)+0x240):0;
    const auto span=enter(Kind::physics_wait,wrapper); wait_original(owner); leave(Kind::physics_wait,span,wrapper);
}
void complete(void* task) {
    // Task is embedded at wrapper+0x148. No dereference after completion publishes its flag.
    const auto wrapper=reinterpret_cast<uintptr_t>(task)-0x148;
    const auto span=enter(Kind::physics_complete,wrapper); complete_original(task); leave(Kind::physics_complete,span,wrapper);
}
void post(void* owner) {
    const auto address=reinterpret_cast<uintptr_t>(owner);
    const auto span=enter(Kind::post_physics,address);
    post_original(owner);
    if(span && enabled.load(std::memory_order_relaxed)) {
        const auto now=GetTickCount64(); auto next=next_body_ms.load(std::memory_order_relaxed);
        if(now>=next && next_body_ms.compare_exchange_strong(next,now+100)) {
            const auto count=body_slot_count(address);
            auto scan=event(Kind::body_scan,0,span,address);
            scan.detail=count;
            if(count) {
                const auto start=body_cursor.load(std::memory_order_relaxed)%count;
                // Rotate through at most 128 slots, copying at most 8 bodies per poll.
                for(uint32_t i=0;i<(std::min)(count,128u) && scan.entity<8;++i) {
                    BodySnapshot body{}; const auto result=read_body(address,static_cast<uint32_t>((start+i)%count),dynamic_vtable,body);
                    ++scan.value;
                    if(result!=BodyRead::ok) {scan.flags|=1u<<static_cast<unsigned>(result);continue;}
                    auto e=event(Kind::body,0,span,body.actor);
                    e.entity=body.handle; e.detail=identity(address,salt);
                    e.value=uint64_t(std::bit_cast<uint32_t>(body.linear_damping))|(uint64_t(std::bit_cast<uint32_t>(body.angular_damping))<<32);
                    e.flags=body.alternate?3:1; buffer.push(e); ++scan.entity;
                }
                body_cursor.store((start+scan.value)%count,std::memory_order_relaxed);
            }
            buffer.push(scan);
        }
    }
    leave(Kind::post_physics,span,address);
}
struct Hook { uint32_t rva; const char* name; std::array<unsigned char,18> prefix; void* detour; void** original; };
const Hook hooks[]={
    {0x1b98950,"movement",{0x48,0x8b,0xc4,0x4c,0x89,0x48,0x20,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x53,0x56,0x57},reinterpret_cast<void*>(&movement),reinterpret_cast<void**>(&move_original)},
    {0x1af20b0,"command_flush",{0x48,0x83,0xec,0x28,0x48,0x8b,0x11,0x45,0x33,0xc9,0x4c,0x8b,0x41,0x08,0x48,0x8d,0x8a,0xe8},reinterpret_cast<void*>(&flush),reinterpret_cast<void**>(&flush_original)},
    {0x19bbda0,"script_fixed",{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c},reinterpret_cast<void*>(&script),reinterpret_cast<void**>(&script_original)},
    {0x18d22c0,"renderer_sync",{0x48,0x83,0xec,0x28,0xe8,0x87,0x0a,0,0,0xba,1,0,0,0,0x48,0x8b,0xc8,0x48},reinterpret_cast<void*>(&renderer),reinterpret_cast<void**>(&renderer_original)},
    {0x2cdba00,"physics_begin",{0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0xd9,0x48,0x8d,0x51,0x20,0x48,0x8b,0x0d,0xec,0x04},reinterpret_cast<void*>(&begin),reinterpret_cast<void**>(&begin_original)},
    {0x2ce5380,"physics_wait",{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0xd1,0x48,0x8b,0x89,0x40,0x02,0},reinterpret_cast<void*>(&wait),reinterpret_cast<void**>(&wait_original)},
    {0x2cdb200,"physics_complete",{0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0x48,0x8b,0x49,0x30,0x48},reinterpret_cast<void*>(&complete),reinterpret_cast<void**>(&complete_original)},
    {0x2ce5310,"post_physics",{0x48,0x8b,0xd1,0x48,0x8b,0x89,0x40,0x02,0x00,0x00,0xe9,0x91,0x6a,0xff,0xff,0xcc,0xcc,0xcc},reinterpret_cast<void*>(&post),reinterpret_cast<void**>(&post_original)},
};
std::string fingerprint(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if(!file || BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) { BCryptCloseAlgorithmProvider(algorithm,0); return {}; }
    std::array<unsigned char,65536> bytes{}; bool ok=true;
    while(file) { file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()); if(file.gcount() && BCryptHashData(hash,bytes.data(),static_cast<ULONG>(file.gcount()),0)<0) {ok=false;break;} }
    std::array<unsigned char,32> digest{};
    ok=ok && file.eof() && BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok) return {};
    std::ostringstream out; for(auto byte:digest) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte); return out.str();
}
}

std::string module_fingerprint(const std::filesystem::path& path) { return fingerprint(path); }
bool Recorder::line(const std::string& text) {
    if(bytes_+text.size()+1>max_bytes-4096) return false;
    output_<<text<<'\n'; bytes_+=text.size()+1; return bool(output_);
}
std::string Recorder::start(const std::filesystem::path& root) {
    wchar_t path[32768]{}; const auto length=GetModuleFileNameW(nullptr,path,32768);
    if(!length || length>=32768 || fingerprint(path)!=fingerprint_expected) return "Engine observer refused: unsupported executable fingerprint";
    wchar_t backend_path[32768]{}; const auto backend=GetModuleHandleW(L"PhysX_64.dll");
    const auto backend_length=backend?GetModuleFileNameW(backend,backend_path,32768):0;
    if(!backend_length || backend_length>=32768 || fingerprint(backend_path)!=physx_expected)
        return "Engine observer refused: unsupported physics backend fingerprint";
    dynamic_vtable=reinterpret_cast<uintptr_t>(backend)+0x15dd88;
    image_base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for(const auto& hook:hooks) if(std::memcmp(reinterpret_cast<void*>(image_base+hook.rva),hook.prefix.data(),hook.prefix.size())!=0)
        return std::string("Engine observer refused: changed or already hooked entry ")+hook.name;
    const auto initialized=MH_Initialize();
    if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return "Engine observer refused: hook initialization failed";
    size_t created=0;
    for(const auto& hook:hooks) {
        const auto result=MH_CreateHook(reinterpret_cast<void*>(image_base+hook.rva),hook.detour,hook.original);
        if(result!=MH_OK) {
            for(size_t i=0;i<created;++i) MH_RemoveHook(reinterpret_cast<void*>(image_base+hooks[i].rva));
            return std::string("Engine observer refused: ")+hook.name+" "+MH_StatusToString(result);
        }
        ++created;
    }
    LARGE_INTEGER frequency{},stamp{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&stamp);
    started_=GetTickCount64(); salt=uint64_t(stamp.QuadPart)^(uint64_t(GetCurrentProcessId())<<32);
    const auto filename="engine-observer-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(started_)+".jsonl";
    // Keep physical bytes equal to the accounting in line(), including newlines.
    output_.open(root/filename,std::ios::out|std::ios::trunc|std::ios::binary);
    if(!output_) {
        for(const auto& hook:hooks) MH_RemoveHook(reinterpret_cast<void*>(image_base+hook.rva));
        return "Engine observer refused: cannot open diagnostic log";
    }
    std::ostringstream header;
    header<<"{\"type\":\"header\",\"schema\":4,\"mode\":\"observe-only\",\"sha256\":\""<<fingerprint_expected
          <<"\",\"physx_sha256\":\""<<physx_expected
          <<"\",\"pid\":"<<GetCurrentProcessId()<<",\"qpc_frequency\":"<<frequency.QuadPart
          <<",\"qpc_origin\":"<<stamp.QuadPart<<",\"max_bytes\":"<<max_bytes<<",\"max_ms\":"<<max_milliseconds
          <<",\"mods_suspended\":true,\"hooks\":[";
    for(size_t i=0;i<std::size(hooks);++i) { if(i)header<<','; header<<"{\"name\":\""<<hooks[i].name<<"\",\"rva\":"<<hooks[i].rva<<'}'; }
    header<<"]}";
    if(!line(header.str())) { finish("io_error"); return "Engine observer refused: cannot write header"; }
    output_.flush(); buffer.open();
    for(const auto& hook:hooks) {
        const auto result=MH_EnableHook(reinterpret_cast<void*>(image_base+hook.rva));
        if(result!=MH_OK) {
            // Already enabled trampolines stay pinned/pass-through. No unsafe removal during a call.
            finish("hook_enable_failed"); return std::string("Engine observer refused: ")+hook.name+" "+MH_StatusToString(result);
        }
    }
    enabled.store(true,std::memory_order_release);
    return "Observe-only engine validation active; mods, noclip and visibility suspended; log: "+filename;
}
void Recorder::finish(const char* reason) {
    enabled.store(false,std::memory_order_release); buffer.close();
    if(output_.is_open()) {
        output_<<"{\"type\":\"end\",\"reason\":\""<<reason<<"\",\"written_events\":"<<written_
               <<",\"dropped\":"<<buffer.dropped()<<",\"capture_boundary_may_split_spans\":true}\n";
        output_.flush(); output_.close();
    }
}
void Recorder::poll() {
    if(!output_.is_open()) return;
    const auto now=GetTickCount64();
    if(now-started_>=max_milliseconds) {finish("time_limit");return;}
    // Drain a bounded amount each worker tick; producers never wait for disk.
    for(unsigned batch=0;batch<16;++batch) {
        const auto count=buffer.drain(batch_.data(),batch_.size());
        for(size_t i=0;i<count;++i) {
            std::ostringstream out; write_event(out,batch_[i]);
            if(!line(out.str())) {finish(output_?"size_limit":"io_error");return;} ++written_;
        }
        if(count<batch_.size()) break;
    }
    if(now-last_stats_>=1000) {
        std::ostringstream out; out<<"{\"type\":\"stats\",\"elapsed_ms\":"<<now-started_<<",\"dropped\":"<<buffer.dropped()
            <<",\"movement_calls\":"<<movement_calls.load()<<",\"player_calls\":"<<player_calls.load()<<",\"attempted\":{";
        for(size_t i=0;i<static_cast<size_t>(Kind::count);++i) {if(i)out<<',';out<<'"'<<names[i]<<"\":"<<buffer.count(static_cast<Kind>(i));}
        out<<"}}";
        if(!line(out.str())) {finish(output_?"size_limit":"io_error");return;} last_stats_=now;
    }
    output_.flush(); if(!output_) finish("io_error");
}
Recorder::~Recorder() { finish("worker_shutdown"); }
#ifdef CRML_OBSERVER_TESTING
bool test_hook_prologues() {
    if(MH_Initialize()!=MH_OK) return false;
    bool ok=true;
    for(const auto& hook:hooks) {
        auto memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
        if(!memory) {ok=false;break;}
        std::memcpy(memory,hook.prefix.data(),hook.prefix.size());
        void* trampoline{};
        const auto status=MH_CreateHook(memory,hook.detour,&trampoline);
        ok=ok && status==MH_OK && trampoline;
        if(status==MH_OK) MH_RemoveHook(memory);
        VirtualFree(memory,0,MEM_RELEASE);
    }
    MH_Uninitialize(); return ok;
}
#endif
}
