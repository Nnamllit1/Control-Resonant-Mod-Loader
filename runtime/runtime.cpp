#include "runtime.h"
#include "action_bindings.h"
#include "physics_selection.h"
#include "semver.h"
#include "mod_storage.h"
#include "mod_settings.h"
#include "mod_feedback.h"
#include "mod_tutorials.h"
#include "mod_drawing.h"
#include "mod_lists.h"
#include <atomic>
#include "mod_actions.h"
#include <wasmtime.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace crml {
static_assert(Semver::parse(CRML_VERSION).has_value(), "VERSION must be a valid semantic version of at most 96 bytes");
const char* runtime_version() noexcept {
    // Retain an identifiable binary marker for packaging consistency checks.
    static constexpr char marker[] = "CRML_VERSION=" CRML_VERSION;
    return marker + sizeof("CRML_VERSION=") - 1;
}
namespace {
template<class T, auto Delete> using Owned = std::unique_ptr<T, decltype(Delete)>;
using Engine = Owned<wasm_engine_t, wasm_engine_delete>;
using Store = Owned<wasmtime_store_t, wasmtime_store_delete>;
using Module = Owned<wasmtime_module_t, wasmtime_module_delete>;
using Linker = Owned<wasmtime_linker_t, wasmtime_linker_delete>;
constexpr uint64_t fuel = 100000;
constexpr size_t max_module = 4 * 1024 * 1024;

void check(wasmtime_error_t* error, wasm_trap_t* trap = nullptr) {
    std::string message;
    wasm_byte_vec_t text{};
    if (error) {
        wasmtime_error_message(error, &text);
        message.assign(text.data, text.size);
        wasm_byte_vec_delete(&text);
        wasmtime_error_delete(error);
    }
    if (trap) {
        wasm_trap_message(trap, &text);
        message.append(text.data, text.size);
        wasm_byte_vec_delete(&text);
        wasm_trap_delete(trap);
    }
    if (!message.empty()) throw std::runtime_error(message);
}
std::string read(const std::filesystem::path& path, size_t limit) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw std::runtime_error("Expected a regular file (no links)");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open file");
    std::string result(limit + 1, '\0');
    file.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<size_t>(file.gcount()));
    if (result.size() > limit) throw std::runtime_error("File exceeds size limit");
    return result;
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
struct Manifest {
    std::string id, module;
    ModMetadata metadata;
    uint32_t capabilities{};
    std::array<uint16_t,CRML_ACTION_COUNT> actions{};
    bool has(uint32_t capability) const noexcept { return (capabilities&capability)!=0; }
};
struct Capability {std::string_view name;uint32_t bit;};
constexpr Capability capabilities[]{
    {"log",CRML_CAP_LOG},{"input.buttons",CRML_CAP_INPUT_BUTTONS},
    {"player.noclip",CRML_CAP_PLAYER_NOCLIP},{"player.visibility",CRML_CAP_PLAYER_VISIBILITY},
    {"physics.damping",CRML_CAP_PHYSICS_DAMPING},{"input.motion",CRML_CAP_INPUT_MOTION},
    {"player.motion",CRML_CAP_PLAYER_MOTION},{"input.actions",CRML_CAP_INPUT_ACTIONS},
    {"player.action_rules",CRML_CAP_ACTION_RULES},{"player.read",CRML_CAP_PLAYER_READ},{"camera.read",CRML_CAP_CAMERA_READ},{"navigation.read",CRML_CAP_NAVIGATION_READ},
    {"ui.read",CRML_CAP_UI_READ},{"ui.activate",CRML_CAP_UI_ACTIVATE},
    {"ui.presentation",CRML_CAP_UI_PRESENTATION},
    {"media.read",CRML_CAP_MEDIA_READ},{"media.skip",CRML_CAP_MEDIA_SKIP},
    {"storage",CRML_CAP_STORAGE},{"settings",CRML_CAP_SETTINGS},{"feedback",CRML_CAP_FEEDBACK},{"tutorials",CRML_CAP_TUTORIALS},{"drawing",CRML_CAP_DRAWING},{"lists",CRML_CAP_LISTS}};
Manifest manifest(const std::filesystem::path& path) {
    std::istringstream input(read(path, 8192));
    std::map<std::string, std::string> fields;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Expected key=value manifest");
        auto key = trim(line.substr(0, equal));
        auto value = trim(line.substr(equal + 1));
        if (key != "id" && key != "abi" && key != "module" && key != "capabilities" && key != "min_runtime" &&
            key != "name" && key != "version" && key != "author" && action_slot(key)<0)
            throw std::runtime_error("Unknown manifest field: " + key);
        if (!fields.emplace(key, value).second) throw std::runtime_error("Duplicate manifest field");
    }
    Manifest m{fields["id"], fields["module"]};
    for(const auto* key:{"name","version","author"})
        if(const auto found=fields.find(key);found!=fields.end() && found->second.empty())
            throw std::runtime_error(std::string("Empty manifest metadata: ")+key);
    m.metadata={fields["name"],fields["version"],fields["author"]};
    if(!m.metadata.valid())throw std::runtime_error("Invalid manifest metadata: name/author need UTF-8 text up to 95 bytes; version needs SemVer up to 96 bytes");
    if (const auto found = fields.find("min_runtime"); found != fields.end()) {
        const auto required = Semver::parse(found->second);
        if (!required) throw std::runtime_error("min_runtime must be a semantic version without v (at most 96 bytes)");
        const auto installed = Semver::parse(runtime_version());
        if (!installed) throw std::runtime_error("Invalid compiled runtime version");
        if (installed->compare(*required) < 0)
            throw std::runtime_error("Requires CRML " + found->second + "; installed " + runtime_version() +
                ". Update from https://github.com/Nnamllit1/Control-Resonant-Mod-Loader/releases");
    }
    std::istringstream requested(fields["capabilities"]);
    std::set<std::string> seen;
    for (std::string cap; std::getline(requested, cap, ',');) {
        cap = trim(cap);
        if (!seen.insert(cap).second) throw std::runtime_error("Duplicate capability");
        const auto found=std::find_if(std::begin(capabilities),std::end(capabilities),[&](const auto& candidate){return candidate.name==cap;});
        if(found==std::end(capabilities)) throw std::runtime_error("Unsupported capability");
        m.capabilities|=found->bit;
    }
    for(const auto& [key,value]:fields) {
        const auto slot=action_slot(key);
        if(slot<0) continue;
        if(!m.has(CRML_CAP_INPUT_ACTIONS)) throw std::runtime_error("Action bindings require input.actions");
        const auto code=action_key(value);
        if(!code && value!="None") throw std::runtime_error("Unsupported action key: "+value);
        m.actions[slot]=code;
    }
    if (!fields["capabilities"].empty() && fields["capabilities"].back() == ',') throw std::runtime_error("Empty capability");
    if (m.id.empty() || m.id.size() > 64 || m.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::runtime_error("Invalid mod id");
    if (fields["abi"] != "1") throw std::runtime_error("Unsupported manifest ABI");
    if (m.module.empty() || m.module.size() > 100 || m.module.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos ||
        std::filesystem::path(m.module).extension() != ".wasm")
        throw std::runtime_error("Module must be a local .wasm filename");
    return m;
}
std::string clean(std::string value) {
    for (auto& c : value) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return value;
}
}

struct Runtime::Impl {
    enum class Phase : size_t { start, abi, init, tick, shutdown, count };
    struct Profile {
        struct Calls {
            uint64_t count{},failures{},total_ns{},max_ns{},fuel_peak{},fuel_errors{};
            std::array<size_t,8> calls_peak{};
        };
        std::string id;
        const char* state="rejected";
        uint64_t compile_ns{},load_ns{};
        std::array<Calls,static_cast<size_t>(Phase::count)> phases{};
        static uint64_t elapsed(std::chrono::steady_clock::time_point start) noexcept {
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            return ns>0?static_cast<uint64_t>(ns):0;
        }
        static void add(uint64_t& value,uint64_t amount) noexcept {
            value=amount>UINT64_MAX-value?UINT64_MAX:value+amount;
        }
    };
    struct ClockState {
        Clock source;
        uint64_t origin{},last{};
        explicit ClockState(Clock clock):source(std::move(clock)) {
            if(!source) source=[] {return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());};
            origin=source();
        }
        uint64_t sample() {
            const auto now=source();
            if(now>=origin) last=std::max(last,now-origin);
            return last;
        }
    };
    struct Mod {
        Manifest info;
        Log* log;
        GuestLog* guest_log{};
        Gameplay* gameplay{};
        Input* input{};
        ModActions* actions{};
        ModStorage* storage{};
        size_t storage_calls{};
        ModSettings* settings{};
        size_t settings_calls{};
        ModFeedback* feedback{};
        size_t feedback_calls{};
        ModTutorials* tutorials{};
        ModDrawing* drawing{};
        ModLists* lists{};
        size_t tutorial_calls{};
        uint64_t owner{};
        size_t gameplay_calls{};
        size_t observation_calls{};
        size_t binding_calls{};
        ClockState* clock{};
        uint64_t callback_ms{};
        Profile* profile{};
        wasm_trap_t* observe() noexcept {
            constexpr char error[]="Observation call budget exceeded";
            return ++observation_calls>8 ? wasmtime_trap_new(error,sizeof(error)-1) : nullptr;
        }
        ~Mod() { if (gameplay) gameplay->release(owner); if(settings)settings->detach(owner); if(feedback)feedback->detach(owner); if(tutorials)tutorials->detach(owner); if(drawing)drawing->detach(owner); if(lists)lists->detach(owner); if(actions)actions->detach(owner); }
        static wasm_trap_t* clock_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t*,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            results[0].kind=WASMTIME_I64;results[0].of.i64=static_cast<int64_t>(mod.callback_ms);
            return nullptr;
        }
        static wasm_trap_t* release_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t*,size_t,wasmtime_val_t*,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.gameplay_calls>8) return wasmtime_trap_new("Gameplay call budget exceeded",29);
            if(mod.gameplay) mod.gameplay->release(mod.owner);
            if(mod.feedback)mod.feedback->cancel(mod.owner);
            if(mod.tutorials)mod.tutorials->cancel(mod.owner);
            if(mod.drawing)mod.drawing->cancel(mod.owner);
            if(mod.lists)mod.lists->cancel(mod.owner);
            return nullptr;
        }
        static wasm_trap_t* capabilities_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t*,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            constexpr uint32_t game_bits=CRML_CAP_INPUT_BUTTONS|CRML_CAP_PLAYER_NOCLIP|CRML_CAP_PLAYER_VISIBILITY|
                CRML_CAP_PHYSICS_DAMPING|CRML_CAP_INPUT_MOTION|CRML_CAP_PLAYER_MOTION|
                CRML_CAP_PLAYER_READ|CRML_CAP_CAMERA_READ|CRML_CAP_UI_READ|CRML_CAP_UI_ACTIVATE|
                CRML_CAP_MEDIA_READ|CRML_CAP_MEDIA_SKIP|CRML_CAP_UI_PRESENTATION|CRML_CAP_NAVIGATION_READ|CRML_CAP_ACTION_RULES;
            uint32_t available=CRML_CAP_LOG|(mod.gameplay?(mod.gameplay->capabilities()&game_bits):0u);
            if(mod.storage && mod.storage->available()) available|=CRML_CAP_STORAGE;
            if(mod.settings) available|=CRML_CAP_SETTINGS;
            if(mod.feedback && mod.feedback->available()) available|=CRML_CAP_FEEDBACK;
            if(mod.tutorials && (mod.tutorials->available(0) || mod.tutorials->available(1))) available|=CRML_CAP_TUTORIALS;
            if(mod.drawing && mod.drawing->available()) available|=CRML_CAP_DRAWING;
            if(mod.lists && mod.lists->available()) available|=CRML_CAP_LISTS;
            if(mod.info.has(CRML_CAP_INPUT_ACTIONS) && mod.input && mod.input->available()) available|=CRML_CAP_INPUT_ACTIONS;
            results[0].kind=WASMTIME_I32;results[0].of.i32=int32_t(mod.info.capabilities&available);
            return nullptr;
        }
        static wasm_trap_t* actions_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t*,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            uint32_t bound{};
            const auto* keys=mod.actions?mod.actions->keys(mod.owner):nullptr;
            if(keys) for(size_t i=0;i<keys->size();++i) if((*keys)[i]) bound|=1u<<i;
            const uint32_t state=keys && mod.input && mod.input->available()?mod.input->sample(*keys):0u;
            results[0].kind=WASMTIME_I32;results[0].of.i32=int32_t(state&bound);
            return nullptr;
        }
        template<bool Bind>
        static wasm_trap_t* input_memory_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,
                                                  size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            if constexpr(Bind) {
                if(++mod.binding_calls>CRML_ACTION_COUNT)return fail("Input binding call budget exceeded");
            } else if(auto* trap=mod.observe())return trap;
            constexpr unsigned offset_arg=Bind?1:0;
            const auto offset=static_cast<uint32_t>(args[offset_arg].of.i32);
            const auto length=static_cast<uint32_t>(args[offset_arg+1].of.i32);
            if constexpr(Bind) {
                if(!length || length>=CRML_INPUT_KEY_NAME_CAPACITY)return fail("Input key name length out of range");
            } else if(length!=sizeof(crml_input_state))return fail("Input output size does not match ABI");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY) {wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || length>size-offset) {wasmtime_extern_delete(&memory);return fail("Input range outside guest memory");}
            auto* bytes=wasmtime_memory_data(context,&memory.of.memory)+offset;
            int result=-1;
            if constexpr(Bind) {
                std::array<char,CRML_INPUT_KEY_NAME_CAPACITY> name{};
                std::memcpy(name.data(),bytes,length);
                if(mod.actions)result=mod.actions->bind(mod.owner,static_cast<uint32_t>(args[0].of.i32),std::string_view(name.data(),length));
            } else {
                crml_input_state state{};
                if(mod.actions)result=mod.actions->read(mod.owner,state);
                if(result==1 && mod.input) {
                    if(const auto* keys=mod.actions->keys(mod.owner))mod.input->read(*keys,state.flags,state.held);
                    state.flags &= CRML_INPUT_AVAILABLE|CRML_INPUT_CONTEXT_KNOWN|
                        CRML_INPUT_FOCUSED|CRML_INPUT_FRESH|CRML_INPUT_EMERGENCY;
                    const auto required=CRML_INPUT_FOCUSED|CRML_INPUT_FRESH;
                    const bool usable=!(state.flags&CRML_INPUT_CONTEXT_KNOWN) ||
                        ((state.flags&required)==required && !(state.flags&CRML_INPUT_EMERGENCY));
                    state.held=((state.flags&CRML_INPUT_AVAILABLE) && usable)?state.held&state.bound:0u;
                }
                std::memcpy(bytes,&state,sizeof(state));
            }
            wasmtime_extern_delete(&memory);
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        size_t log_bytes = 0;
        size_t log_calls = 0;
        bool alive = true;
        Store store{nullptr, wasmtime_store_delete};
        wasmtime_instance_t instance{};
        wasmtime_func_t init{}, tick{}, stop{};
        bool has_tick = false, has_stop = false;
        wasmtime_context_t* context() { return wasmtime_store_context(store.get()); }
        void budget() {
            callback_ms=clock->sample();
            log_bytes = 0;
            log_calls = 0;
            gameplay_calls = 0;
            observation_calls = 0;
            binding_calls = 0;
            storage_calls = 0;
            settings_calls = 0;
            feedback_calls = 0;
            tutorial_calls = 0;
            check(wasmtime_context_set_fuel(context(), fuel));
        }
        bool function(const char* name, const std::vector<wasm_valkind_t>& params,
                      const std::vector<wasm_valkind_t>& results, wasmtime_func_t& result, bool required) {
            wasmtime_extern_t item{};
            if (!wasmtime_instance_export_get(context(), &instance, name, strlen(name), &item)) {
                if (required) throw std::runtime_error(std::string("Missing export: ") + name);
                return false;
            }
            if (item.kind != WASMTIME_EXTERN_FUNC) {
                wasmtime_extern_delete(&item);
                throw std::runtime_error("Lifecycle export must be a function");
            }
            result = item.of.func;
            wasmtime_extern_delete(&item);
            Owned<wasm_functype_t, wasm_functype_delete> type(wasmtime_func_type(context(), &result), wasm_functype_delete);
            auto matches = [](const wasm_valtype_vec_t* actual, const std::vector<wasm_valkind_t>& wanted) {
                if (actual->size != wanted.size()) return false;
                for (size_t i = 0; i < wanted.size(); ++i) if (wasm_valtype_kind(actual->data[i]) != wanted[i]) return false;
                return true;
            };
            if (!matches(wasm_functype_params(type.get()), params) || !matches(wasm_functype_results(type.get()), results))
                throw std::runtime_error(std::string("Wrong signature: ") + name);
            return true;
        }
        void record(Phase phase,std::chrono::steady_clock::time_point started,bool failed) noexcept {
            auto& stats=profile->phases[static_cast<size_t>(phase)];
            const auto elapsed=Profile::elapsed(started);
            Profile::add(stats.count,1);Profile::add(stats.failures,failed?1:0);
            Profile::add(stats.total_ns,elapsed);stats.max_ns=std::max(stats.max_ns,elapsed);
            uint64_t remaining{};
            if(auto* error=wasmtime_context_get_fuel(context(),&remaining)) {
                wasmtime_error_delete(error);Profile::add(stats.fuel_errors,1);
            } else stats.fuel_peak=std::max(stats.fuel_peak,fuel-std::min(fuel,remaining));
            const std::array<size_t,8> peaks{observation_calls,gameplay_calls,binding_calls,
                storage_calls,settings_calls,feedback_calls,log_calls,tutorial_calls};
            for(size_t i=0;i<peaks.size();++i) stats.calls_peak[i]=std::max(stats.calls_peak[i],peaks[i]);
        }
        void call(Phase phase,wasmtime_func_t& fn, const wasmtime_val_t* args = nullptr, size_t nargs = 0,
                  wasmtime_val_t* results = nullptr, size_t nresults = 0) {
            budget();
            wasm_trap_t* trap = nullptr;
            const auto started=std::chrono::steady_clock::now();
            auto* error = wasmtime_func_call(context(), &fn, args, nargs, results, nresults, &trap);
            record(phase,started,error || trap);
            check(error, trap);
        }
        template<int Operation>
        static wasm_trap_t* list_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            auto& mod=*static_cast<Mod*>(data);
            if constexpr(Operation==1){if(auto* trap=mod.observe())return trap;}
            else if(++mod.gameplay_calls>8)return fail("Gameplay call budget exceeded");
            int64_t result=-1;
            if constexpr(Operation==2) {
                try {if(mod.lists)result=mod.lists->hide(mod.owner);}catch(...){return fail("List service failed");}
            } else {
                const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
                constexpr size_t expected=Operation==0?sizeof(crml_list_page):sizeof(crml_list_event);
                if(length!=expected)return fail("Invalid list buffer size");
                wasmtime_extern_t memory{};
                if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
                if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
                auto* context=wasmtime_caller_context(caller);
                const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
                if(offset>size || length>size-offset){wasmtime_extern_delete(&memory);return fail("List buffer outside guest memory");}
                auto* bytes=wasmtime_memory_data(context,&memory.of.memory)+offset;
                try {
                    if constexpr(Operation==0) {crml_list_page page{};std::memcpy(&page,bytes,sizeof(page));if(mod.lists)result=mod.lists->publish(mod.owner,page);}
                    else {crml_list_event event{};if(mod.lists)result=mod.lists->next(mod.owner,event);std::memcpy(bytes,&event,sizeof(event));}
                } catch(...) {wasmtime_extern_delete(&memory);return fail("List service failed");}
                wasmtime_extern_delete(&memory);
            }
            if constexpr(Operation==0){results[0].kind=WASMTIME_I64;results[0].of.i64=result;}
            else {results[0].kind=WASMTIME_I32;results[0].of.i32=static_cast<int32_t>(result);}
            return nullptr;
        }
        template<bool Publish>
        static wasm_trap_t* drawing_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.gameplay_calls>8)return fail("Gameplay call budget exceeded");
            crml_drawing_frame frame{};
            if constexpr(Publish) {
                const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
                if(length!=sizeof(frame))return fail("Invalid drawing frame size");
                wasmtime_extern_t memory{};
                if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
                if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
                auto* context=wasmtime_caller_context(caller);
                const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
                if(offset>size || length>size-offset){wasmtime_extern_delete(&memory);return fail("Drawing frame outside guest memory");}
                std::memcpy(&frame,wasmtime_memory_data(context,&memory.of.memory)+offset,sizeof(frame));
                wasmtime_extern_delete(&memory);
            }
            int result=-1;
            try {if(mod.drawing) {
                if constexpr(Publish)result=mod.drawing->publish(mod.owner,frame);
                else result=mod.drawing->hide(mod.owner);
            }} catch(...) {return fail("Drawing service failed");}
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        template<unsigned Operation>
        static wasm_trap_t* annotations_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};auto& mod=*static_cast<Mod*>(data);
            if constexpr((Operation==1||Operation==5||Operation==6)){if(auto* trap=mod.observe())return trap;}
            else if(++mod.gameplay_calls>8)return fail("Gameplay call budget exceeded");
            using Frame=std::conditional_t<Operation==4,crml_map_annotations_v3,std::conditional_t<Operation==3,crml_map_annotations_v2,crml_map_annotations>>;
            using Event=std::conditional_t<Operation==6,crml_map_annotation_status,std::conditional_t<Operation==5,crml_map_annotation_event_v2,crml_map_annotation_event>>;
            Frame frame{};Event event{};unsigned char* bytes{};
            if constexpr(Operation!=2){
                constexpr size_t expected=(Operation==1||Operation==5||Operation==6)?sizeof(event):sizeof(frame);
                const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
                if(length!=expected)return fail("Invalid map annotation descriptor size");
                wasmtime_extern_t memory{};if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
                if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
                auto* context=wasmtime_caller_context(caller);const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
                if(offset>size||length>size-offset){wasmtime_extern_delete(&memory);return fail("Map annotation outside guest memory");}
                bytes=wasmtime_memory_data(context,&memory.of.memory)+offset;
                if constexpr(Operation==0||Operation==3||Operation==4)std::memcpy(&frame,bytes,sizeof(frame));wasmtime_extern_delete(&memory);
            }
            int result=-1;try{if(mod.drawing){
                if constexpr(Operation==0)result=mod.drawing->annotations_publish(mod.owner,frame);
                if constexpr(Operation==3)result=frame.version==2?mod.drawing->annotations_publish_v2(mod.owner,frame):-3;
                if constexpr(Operation==1)result=mod.drawing->annotations_next(mod.owner,event);
                if constexpr(Operation==4)result=mod.drawing->annotations_publish_v3(mod.owner,frame);
                if constexpr(Operation==5)result=mod.drawing->annotations_next_v2(mod.owner,event);
                if constexpr(Operation==6)result=mod.drawing->annotations_status(mod.owner,event);
                if constexpr(Operation==2)result=mod.drawing->annotations_hide(mod.owner);
            }}catch(...){return fail("Map annotations service failed");}
            if constexpr((Operation==1||Operation==5||Operation==6))std::memcpy(bytes,&event,sizeof(event));
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        template<unsigned Operation>
        static wasm_trap_t* map_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            auto& mod=*static_cast<Mod*>(data);
            if constexpr((Operation==0||Operation==3||Operation==5)){if(auto* trap=mod.observe())return trap;}
            else if(++mod.gameplay_calls>8)return fail("Gameplay call budget exceeded");
            crml_map_frame frame{};crml_map_state state{};crml_map_projection projection{};
            unsigned char* bytes{};
            if constexpr((Operation<2||Operation==3||Operation==5)){
                constexpr size_t expected=Operation==5?sizeof(projection):(Operation==0||Operation==3)?sizeof(state):sizeof(frame);
                const auto offset=static_cast<uint32_t>(args[Operation==3?1:0].of.i32),length=static_cast<uint32_t>(args[Operation==3?2:1].of.i32);
                if(length!=expected)return fail("Invalid map descriptor size");
                wasmtime_extern_t memory{};
                if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
                if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
                auto* context=wasmtime_caller_context(caller);const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
                if(offset>size||length>size-offset){wasmtime_extern_delete(&memory);return fail("Map descriptor outside guest memory");}
                bytes=wasmtime_memory_data(context,&memory.of.memory)+offset;
                if constexpr(Operation==1)std::memcpy(&frame,bytes,sizeof(frame));
                wasmtime_extern_delete(&memory);
            }
            int result=-1;
            try {if(mod.drawing){
                if constexpr(Operation==0)result=mod.drawing->map_read(mod.owner,state);
                if constexpr(Operation==5)result=mod.drawing->projection_read(mod.owner,projection);
                if constexpr(Operation==3)result=mod.drawing->map_read_target(mod.owner,static_cast<uint32_t>(args[0].of.i32),state);
                if constexpr(Operation==4)result=mod.drawing->map_hide_target(mod.owner,static_cast<uint32_t>(args[0].of.i32));
                if constexpr(Operation==1)result=mod.drawing->map_publish(mod.owner,frame);
                if constexpr(Operation==2)result=mod.drawing->map_hide(mod.owner);
            }}catch(...){return fail("Map drawing service failed");}
            if constexpr((Operation==0||Operation==3))std::memcpy(bytes,&state,sizeof(state));
            if constexpr(Operation==5)std::memcpy(bytes,&projection,sizeof(projection));
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        static wasm_trap_t* tutorial_show_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.tutorial_calls>16)return fail("Tutorial call budget exceeded");
            const auto title=static_cast<uint32_t>(args[1].of.i32),title_size=static_cast<uint32_t>(args[2].of.i32);
            const auto body=static_cast<uint32_t>(args[3].of.i32),body_size=static_cast<uint32_t>(args[4].of.i32);
            if(title_size>CRML_TUTORIAL_TITLE_MAX || body_size>CRML_TUTORIAL_BODY_MAX)return fail("Invalid tutorial text size");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(title>size || title_size>size-title || body>size || body_size>size-body) {
                wasmtime_extern_delete(&memory);return fail("Tutorial buffer outside guest memory");
            }
            auto* bytes=wasmtime_memory_data(context,&memory.of.memory);int64_t result=-1;
            try {if(mod.tutorials)result=mod.tutorials->show(mod.owner,static_cast<uint32_t>(args[0].of.i32),
                {reinterpret_cast<const char*>(bytes+title),title_size},{reinterpret_cast<const char*>(bytes+body),body_size},
                static_cast<uint32_t>(args[5].of.i32));} catch(...) {wasmtime_extern_delete(&memory);return fail("Tutorial service failed");}
            wasmtime_extern_delete(&memory);results[0].kind=WASMTIME_I64;results[0].of.i64=result;return nullptr;
        }
        static wasm_trap_t* tutorial_present_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.tutorial_calls>16)return fail("Tutorial call budget exceeded");
            const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
            if(length!=sizeof(crml_tutorial_page))return fail("Invalid tutorial descriptor size");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || length>size-offset){wasmtime_extern_delete(&memory);return fail("Tutorial descriptor outside guest memory");}
            crml_tutorial_page page{};
            std::memcpy(&page,wasmtime_memory_data(context,&memory.of.memory)+offset,sizeof(page));
            wasmtime_extern_delete(&memory);
            int64_t result=-1;
            try {if(mod.tutorials)result=mod.tutorials->present(mod.owner,page);}
            catch(...) {return fail("Tutorial service failed");}
            results[0].kind=WASMTIME_I64;results[0].of.i64=result;return nullptr;
        }
        template<int Operation>
        static wasm_trap_t* tutorial_receipt_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.tutorial_calls>16)return wasmtime_trap_new("Tutorial call budget exceeded",29);
            int result=-1;
            try {if(mod.tutorials) {
                if constexpr(Operation==0)result=mod.tutorials->status(mod.owner,static_cast<uint64_t>(args[0].of.i64));
                else if constexpr(Operation==1)result=mod.tutorials->dismiss(mod.owner,static_cast<uint64_t>(args[0].of.i64));
                else {const auto kind=static_cast<uint32_t>(args[0].of.i32);result=kind>CRML_TUTORIAL_PROMPT?-3:mod.tutorials->available(kind)?1:0;}
            }} catch(...) {return wasmtime_trap_new("Tutorial service failed",23);}
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        static wasm_trap_t* feedback_show_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* text){return wasmtime_trap_new(text,strlen(text));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.feedback_calls>16)return fail("Feedback call budget exceeded");
            const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
            if(length>CRML_FEEDBACK_TEXT_MAX)return fail("Invalid feedback text size");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || length>size-offset){wasmtime_extern_delete(&memory);return fail("Feedback buffer outside guest memory");}
            int64_t result=-1;
            try {
                const auto* bytes=wasmtime_memory_data(context,&memory.of.memory);
                if(mod.feedback)result=mod.feedback->show(mod.owner,{reinterpret_cast<const char*>(bytes+offset),length},
                    static_cast<uint32_t>(args[2].of.i32),static_cast<uint32_t>(args[3].of.i32));
            } catch(...) {result=-5;}
            wasmtime_extern_delete(&memory);
            results[0].kind=WASMTIME_I64;results[0].of.i64=result;return nullptr;
        }
        template<bool Dismiss>
        static wasm_trap_t* feedback_receipt_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            constexpr std::string_view budget_error="Feedback call budget exceeded";
            if(++mod.feedback_calls>16)return wasmtime_trap_new(budget_error.data(),budget_error.size());
            int result=-1;
            try {if(mod.feedback) {
                const auto ticket=static_cast<uint64_t>(args[0].of.i64);
                if constexpr(Dismiss)result=mod.feedback->dismiss(mod.owner,ticket);
                else result=mod.feedback->status(mod.owner,ticket);
            }}catch(...) {result=-5;}
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        template<bool Define>
        static wasm_trap_t* settings_memory_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,
                                                     size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* message){return wasmtime_trap_new(message,strlen(message));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.settings_calls>64)return fail("Settings call budget exceeded");
            const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
            if constexpr(Define) {if(length!=sizeof(crml_setting_definition))return fail("Invalid setting definition size");}
            else if(length>sizeof(crml_setting_value)*CRML_SETTINGS_MAX || length%sizeof(crml_setting_value))return fail("Invalid settings output size");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || length>size-offset){wasmtime_extern_delete(&memory);return fail("Settings buffer outside guest memory");}
            auto* bytes=wasmtime_memory_data(context,&memory.of.memory);
            int result=-1;
            try {
                if(mod.settings) {
                    if constexpr(Define) {
                        crml_setting_definition definition{};std::memcpy(&definition,bytes+offset,sizeof(definition));
                        result=mod.settings->define(mod.owner,definition);
                    } else {
                        std::array<crml_setting_value,CRML_SETTINGS_MAX> values{};
                        result=mod.settings->read(mod.owner,std::span(values).first(length/sizeof(crml_setting_value)));
                        if(result>0)std::memcpy(bytes+offset,values.data(),static_cast<size_t>(result)*sizeof(crml_setting_value));
                    }
                }
            } catch(...) {result=-5;}
            wasmtime_extern_delete(&memory);
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        static wasm_trap_t* settings_set_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.settings_calls>64)return wasmtime_trap_new("Settings call budget exceeded",29);
            results[0].kind=WASMTIME_I32;results[0].of.i32=-1;
            try {if(mod.settings)results[0].of.i32=mod.settings->set(mod.owner,static_cast<uint32_t>(args[0].of.i32),args[1].of.f64,static_cast<uint64_t>(args[2].of.i64));}
            catch(...) {results[0].of.i32=-5;}
            return nullptr;
        }
        template<unsigned Operation>
        static wasm_trap_t* settings_text_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* message){return wasmtime_trap_new(message,strlen(message));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.settings_calls>64)return fail("Settings call budget exceeded");
            constexpr unsigned pointer_arg=Operation==0?0:1;
            const auto offset=static_cast<uint32_t>(args[pointer_arg].of.i32),length=static_cast<uint32_t>(args[pointer_arg+1].of.i32);
            if constexpr(Operation==0) {if(length!=sizeof(crml_text_setting_definition))return fail("Invalid text setting definition size");}
            if constexpr(Operation==1) {if(length!=sizeof(crml_text_setting_value))return fail("Invalid text setting output size");}
            if constexpr(Operation==2) {if(length>CRML_SETTING_TEXT_MAX)return fail("Text setting length exceeds limit");}
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory))return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY){wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || length>size-offset){wasmtime_extern_delete(&memory);return fail("Settings buffer outside guest memory");}
            auto* bytes=wasmtime_memory_data(context,&memory.of.memory);int result=-1;
            try {if(mod.settings) {
                if constexpr(Operation==0) {
                    crml_text_setting_definition definition{};std::memcpy(&definition,bytes+offset,sizeof(definition));
                    result=mod.settings->define_text(mod.owner,definition);
                } else if constexpr(Operation==1) {
                    crml_text_setting_value value{};result=mod.settings->read_text(mod.owner,static_cast<uint32_t>(args[0].of.i32),value);
                    if(result==1)std::memcpy(bytes+offset,&value,sizeof(value));
                } else {
                    result=mod.settings->set_text(mod.owner,static_cast<uint32_t>(args[0].of.i32),
                        std::string_view(reinterpret_cast<const char*>(bytes+offset),length),static_cast<uint64_t>(args[3].of.i64));
                }
            }}catch(...){result=-5;}
            wasmtime_extern_delete(&memory);results[0].kind=WASMTIME_I32;results[0].of.i32=result;return nullptr;
        }
        template<bool Write>
        static wasm_trap_t* storage_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,
                                            size_t,wasmtime_val_t* results,size_t) noexcept {
            auto fail=[](const char* message){return wasmtime_trap_new(message,strlen(message));};
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.storage_calls>16) return fail("Storage call budget exceeded");
            const auto offset=static_cast<uint32_t>(args[0].of.i32),length=static_cast<uint32_t>(args[1].of.i32);
            if(length>ModStorage::limit) return fail("Storage length exceeds 65536 bytes");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory)) return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY) {wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || length>size-offset) {wasmtime_extern_delete(&memory);return fail("Storage buffer outside guest memory");}
            auto* bytes=wasmtime_memory_data(context,&memory.of.memory);
            int result=-1;
            try {
                if(mod.storage) {
                    std::span<unsigned char> buffer(length?bytes+offset:bytes,length);
                    if constexpr(Write) result=mod.storage->write(mod.info.id,buffer);
                    else result=mod.storage->read(mod.info.id,buffer);
                }
            } catch(...) {result=-5;}
            wasmtime_extern_delete(&memory);
            results[0].kind=WASMTIME_I32;results[0].of.i32=result;
            return nullptr;
        }
        static wasm_trap_t* storage_status_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t*,size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.storage_calls>16) return wasmtime_trap_new("Storage call budget exceeded",28);
            results[0].kind=WASMTIME_I32;results[0].of.i32=-1;
            try {if(mod.storage)results[0].of.i32=mod.storage->status(mod.info.id);} catch(...) {results[0].of.i32=-5;}
            return nullptr;
        }
        static wasm_trap_t* noclip_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t* args,
                                          size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod = *static_cast<Mod*>(data);
            const auto speed = args[0].of.f32;
            if (!std::isfinite(speed) || speed < 0.25f || speed > 20.0f || ++mod.gameplay_calls > 8) {
                constexpr char error[] = "Noclip speed or call budget exceeded";
                return wasmtime_trap_new(error, sizeof(error)-1);
            }
            results[0].kind = WASMTIME_I32;
            results[0].of.i32 = mod.gameplay ? mod.gameplay->noclip_poll(mod.owner, speed) : -1;
            return nullptr;
        }
        static wasm_trap_t* input_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t*,
                                         size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?static_cast<int32_t>(mod.gameplay->input_buttons() & 3u):0;
            return nullptr;
        }
        static wasm_trap_t* action_rule_set_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,
                                                    size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.gameplay_calls>8) {
                constexpr char error[]="Gameplay call budget exceeded";
                return wasmtime_trap_new(error,sizeof(error)-1);
            }
            const auto actions=static_cast<uint32_t>(args[0].of.i32);
            const auto restrictions=static_cast<uint32_t>(args[1].of.i32);
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=(actions&~CRML_RULE_ACTION_ALL) ||
                (actions?restrictions!=CRML_RULE_RESTRICTION_AREA:restrictions!=0)?-3:
                mod.gameplay?mod.gameplay->action_rule_set(mod.owner,actions,restrictions):-1;
            return nullptr;
        }
        static wasm_trap_t* visibility_set_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t* args,
                                                  size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            const int hidden=args[0].of.i32;
            if((hidden!=0 && hidden!=1) || ++mod.gameplay_calls>8) {
                constexpr char error[]="Visibility argument or gameplay call budget exceeded";
                return wasmtime_trap_new(error,sizeof(error)-1);
            }
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?mod.gameplay->visibility_set(mod.owner,hidden==1):-1;
            return nullptr;
        }
        static wasm_trap_t* visibility_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t*,
                                              size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.gameplay_calls>8) {
                constexpr char error[]="Gameplay call budget exceeded";
                return wasmtime_trap_new(error,sizeof(error)-1);
            }
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?mod.gameplay->visibility_poll(mod.owner):-1;
            return nullptr;
        }
        template<unsigned Op>
        static wasm_trap_t* physics_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t* args,
                                            size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if constexpr(Op==1 || Op==3) {
                if(auto* trap=mod.observe()) return trap;
            } else if(++mod.gameplay_calls>8) {
                constexpr char error[]="Gameplay call budget exceeded";
                return wasmtime_trap_new(error,sizeof(error)-1);
            }
            if constexpr(Op==2) {
                if(!std::isfinite(args[1].of.f32) || args[1].of.f32<0 || args[1].of.f32>8
                   || args[2].of.i32<1 || args[2].of.i32>5000) {
                    constexpr char error[]="Physics damping value or duration out of range";
                    return wasmtime_trap_new(error,sizeof(error)-1);
                }
            }
            if constexpr(Op==5) {
                const physics::SelectionQuery query{{args[0].of.f32,args[1].of.f32,args[2].of.f32},args[3].of.f32};
                if(!query.valid()) {
                    constexpr char error[]="Physics selection region out of range";
                    return wasmtime_trap_new(error,sizeof(error)-1);
                }
            }
            if constexpr(Op==1) {
                results[0].kind=WASMTIME_I64;
                results[0].of.i64=mod.gameplay?static_cast<int64_t>(mod.gameplay->physics_target(mod.owner)):0;
            } else {
                int result=-1;
                if(mod.gameplay) {
                    if constexpr(Op==0) result=mod.gameplay->physics_select(mod.owner);
                    if constexpr(Op==2) result=mod.gameplay->physics_apply(mod.owner,static_cast<uint64_t>(args[0].of.i64),args[1].of.f32,static_cast<uint32_t>(args[2].of.i32));
                    if constexpr(Op==3) result=mod.gameplay->physics_status(mod.owner);
                    if constexpr(Op==4) result=mod.gameplay->physics_restore(mod.owner);
                    if constexpr(Op==5) result=mod.gameplay->physics_select_near(mod.owner,args[0].of.f32,args[1].of.f32,args[2].of.f32,args[3].of.f32);
                }
                results[0].kind=WASMTIME_I32;results[0].of.i32=result;
            }
            return nullptr;
        }
        static wasm_trap_t* motion_input_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            results[0].kind=WASMTIME_I32;results[0].of.i32=mod.gameplay?int32_t(mod.gameplay->input_motion()&255u):0;
            return nullptr;
        }
        static wasm_trap_t* motion_set_callback(void* data, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            const auto enabled=args[0].of.i32;
            const float x=args[1].of.f32,y=args[2].of.f32,z=args[3].of.f32;
            if(++mod.gameplay_calls>8 || (enabled!=0 && enabled!=1) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::hypot(x,y,z)>20.f) {
                constexpr char error[]="Invalid movement request or call budget exceeded";
                return wasmtime_trap_new(error,sizeof(error)-1);
            }
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?mod.gameplay->motion_set(mod.owner,enabled!=0,x,y,z):-1;
            return nullptr;
        }
        static wasm_trap_t* motion_camera_callback(void* data, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory)) return wasmtime_trap_new("Missing guest memory",20);
            if(memory.kind!=WASMTIME_EXTERN_MEMORY) {wasmtime_extern_delete(&memory);return wasmtime_trap_new("Invalid guest memory",20);}
            const auto offset=static_cast<uint32_t>(args[0].of.i32);
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || size-offset<8) {wasmtime_extern_delete(&memory);return wasmtime_trap_new("Camera output outside guest memory",34);}
            float right[2]{};
            int status=mod.gameplay?mod.gameplay->motion_camera(right):-1;
            if(status!=1) right[0]=right[1]=0;
            std::memcpy(wasmtime_memory_data(context,&memory.of.memory)+offset,right,sizeof(right));
            wasmtime_extern_delete(&memory);
            results[0].kind=WASMTIME_I32;results[0].of.i32=status;return nullptr;
        }
        template<class State, unsigned Op>
        static wasm_trap_t* state_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,
                                          size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            auto fail=[](const char* message) {return wasmtime_trap_new(message,strlen(message));};
            if(auto* trap=mod.observe()) return trap;
            constexpr unsigned offset_arg=Op==2?1:0;
            if(static_cast<uint32_t>(args[offset_arg+1].of.i32)!=sizeof(State))
                return fail("Snapshot output size does not match ABI");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory)) return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY) {wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            const auto offset=static_cast<uint32_t>(args[offset_arg].of.i32);
            auto* context=wasmtime_caller_context(caller);
            const auto length=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>length || length-offset<sizeof(State)) {
                wasmtime_extern_delete(&memory);return fail("Snapshot output outside guest memory");
            }
            State state{};
            int status=-1;
            if(mod.gameplay) {
                if constexpr(Op==0) status=mod.gameplay->player_read(state);
                if constexpr(Op==1) status=mod.gameplay->camera_read(state);
                if constexpr(Op==2) status=mod.gameplay->physics_read(mod.owner,static_cast<uint64_t>(args[0].of.i64),state);
                if constexpr(Op==3) status=mod.gameplay->ui_read(state);
                if constexpr(Op==4) status=mod.gameplay->media_read(state);
                if constexpr(Op==5) status=mod.gameplay->motion_read(mod.owner,state);
                if constexpr(Op==6) status=mod.gameplay->visibility_read(mod.owner,state);
                if constexpr(Op==7) status=mod.gameplay->navigation_read(state);
                if constexpr(Op==8) status=mod.gameplay->navigation_read_v2(state);
                if constexpr(Op==9) status=mod.gameplay->action_rule_read(mod.owner,state);
            }
            if(status!=1) state={};
            std::memcpy(wasmtime_memory_data(context,&memory.of.memory)+offset,&state,sizeof(state));
            wasmtime_extern_delete(&memory);
            results[0].kind=WASMTIME_I32;results[0].of.i32=status;
            return nullptr;
        }
        template<bool Tracked=false> static wasm_trap_t* ui_activate_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,
                                                 size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            auto fail=[](const char* message) {return wasmtime_trap_new(message,strlen(message));};
            if(++mod.gameplay_calls>8) return fail("Gameplay call budget exceeded");
            const auto generation=static_cast<uint64_t>(args[0].of.i64);
            const auto action=static_cast<uint32_t>(args[1].of.i32);
            if(!generation || action!=CRML_UI_ACTION_CONTINUE) return fail("Invalid UI action arguments");
            if constexpr(Tracked) {
                results[0].kind=WASMTIME_I64;
                results[0].of.i64=mod.gameplay?mod.gameplay->ui_action_submit(mod.owner,generation,action):-1;
            } else {
                results[0].kind=WASMTIME_I32;
                results[0].of.i32=mod.gameplay?mod.gameplay->ui_activate(mod.owner,generation,action):-1;
            }
            return nullptr;
        }
        static wasm_trap_t* ui_action_status_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,
                                                    size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(auto* trap=mod.observe()) return trap;
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?mod.gameplay->ui_action_status(mod.owner,static_cast<uint64_t>(args[0].of.i64)):-1;
            return nullptr;
        }
        static wasm_trap_t* ui_present_callback(void* data,wasmtime_caller_t* caller,const wasmtime_val_t* args,
                                                size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            auto fail=[](const char* message) {return wasmtime_trap_new(message,strlen(message));};
            if(++mod.gameplay_calls>8) return fail("Gameplay call budget exceeded");
            const auto generation=static_cast<uint64_t>(args[0].of.i64);
            const auto kind=static_cast<uint32_t>(args[1].of.i32);
            const auto offset=static_cast<uint32_t>(args[2].of.i32);
            const auto length=static_cast<uint32_t>(args[3].of.i32);
            const auto hidden=static_cast<uint32_t>(args[4].of.i32);
            const auto duration=static_cast<uint32_t>(args[5].of.i32);
            if(!generation || (kind!=CRML_UI_TARGET_ID && kind!=CRML_UI_TARGET_CLASS) ||
               !length || length>CRML_UI_TARGET_NAME_MAX || hidden>1 ||
               (hidden?(!duration || duration>CRML_UI_PRESENT_MAX_MS):duration!=0))
                return fail("Invalid UI presentation arguments");
            wasmtime_extern_t memory{};
            if(!wasmtime_caller_export_get(caller,"memory",6,&memory)) return fail("Missing guest memory");
            if(memory.kind!=WASMTIME_EXTERN_MEMORY) {wasmtime_extern_delete(&memory);return fail("Invalid guest memory");}
            auto* context=wasmtime_caller_context(caller);
            const auto size=wasmtime_memory_data_size(context,&memory.of.memory);
            if(offset>size || size-offset<length) {
                wasmtime_extern_delete(&memory);return fail("UI presentation name outside guest memory");
            }
            std::array<char,CRML_UI_TARGET_NAME_MAX> name{};
            std::memcpy(name.data(),wasmtime_memory_data(context,&memory.of.memory)+offset,length);
            wasmtime_extern_delete(&memory);
            for(uint32_t i=0;i<length;++i) {
                const auto c=static_cast<unsigned char>(name[i]);
                if(!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-'))
                    return fail("Invalid UI presentation name");
            }
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?mod.gameplay->ui_present(mod.owner,generation,kind,
                std::string_view(name.data(),length),hidden!=0,duration):-1;
            return nullptr;
        }
        static wasm_trap_t* media_skip_callback(void* data,wasmtime_caller_t*,const wasmtime_val_t* args,
                                                size_t,wasmtime_val_t* results,size_t) noexcept {
            auto& mod=*static_cast<Mod*>(data);
            if(++mod.gameplay_calls>8) return wasmtime_trap_new("Gameplay call budget exceeded",29);
            const auto generation=static_cast<uint64_t>(args[0].of.i64);
            if(!generation) return wasmtime_trap_new("Invalid media generation",24);
            results[0].kind=WASMTIME_I32;
            results[0].of.i32=mod.gameplay?mod.gameplay->media_skip(mod.owner,generation):-1;
            return nullptr;
        }
        static wasm_trap_t* log_callback(void* data, wasmtime_caller_t* caller, const wasmtime_val_t* args,
                                         size_t, wasmtime_val_t*, size_t) noexcept {
            auto& mod = *static_cast<Mod*>(data);
            auto fail = [](const char* message) { return wasmtime_trap_new(message, strlen(message)); };
            try {
                const auto level = args[0].of.i32;
                const auto offset = static_cast<uint32_t>(args[1].of.i32);
                const auto length = static_cast<uint32_t>(args[2].of.i32);
                if (level < 0 || level > 3 || length > 4096 || mod.log_bytes + length > 16384 || ++mod.log_calls > 32)
                    return fail("Log budget or argument limit exceeded");
                wasmtime_extern_t memory{};
                if (!wasmtime_caller_export_get(caller, "memory", 6, &memory)) return fail("Missing guest memory");
                if (memory.kind != WASMTIME_EXTERN_MEMORY) {
                    wasmtime_extern_delete(&memory);
                    return fail("Invalid guest memory");
                }
                auto* context = wasmtime_caller_context(caller);
                const size_t size = wasmtime_memory_data_size(context, &memory.of.memory);
                if (offset > size || length > size - offset) {
                    wasmtime_extern_delete(&memory);
                    return fail("Log pointer outside guest memory");
                }
                auto* bytes = wasmtime_memory_data(context, &memory.of.memory);
                std::string text;
                if (length) text.assign(reinterpret_cast<char*>(bytes + offset), length);
                wasmtime_extern_delete(&memory);
                mod.log_bytes += length;
                text=clean(std::move(text));
                if(mod.guest_log && *mod.guest_log) (*mod.guest_log)(mod.info.id,level,text);
                else {
                    constexpr const char* levels[]{"debug","info","warning","error"};
                    (*mod.log)("[" + mod.info.id + "] [" + levels[level] + "] " + text);
                }
                return nullptr;
            } catch (...) { return fail("Host log failed"); }
        }
    };
    Log log;
    GuestLog guest_log;
    Gameplay* gameplay{};
    Input* input{};
    ModStorage* storage{};
    ModSettings own_settings;
    ModSettings* settings{};
    ModFeedback own_feedback;
    ModFeedback* feedback{};
    ModTutorials own_tutorials;
    ModTutorials* tutorials{};
    ModDrawing own_drawing;
    ModDrawing* drawing{};
    ModLists own_lists;
    ModLists* lists{};
    ModActions actions;
    ClockState clock;
    static uint64_t allocate_owner() {
        // Services may be shared by independent Runtime instances. Never reuse
        // an owner after shutdown or let one runtime detach another's leases.
        static std::atomic<uint64_t> next{1};
        auto owner=next.load(std::memory_order_relaxed);
        for(;;) {
            if(owner==UINT64_MAX)throw std::runtime_error("Mod owner identity exhausted");
            if(next.compare_exchange_weak(owner,owner+1,std::memory_order_relaxed))return owner;
        }
    }
    Engine engine{nullptr, wasm_engine_delete};
    std::vector<std::unique_ptr<Mod>> mods;
    std::array<Profile,32> profiles{};
    size_t profile_count{};
    size_t failed = 0;
    bool loaded = false;
    explicit Impl(Log sink, Gameplay* game, Input* keys, GuestLog guest_sink, ModStorage* storage_service, ModSettings* settings_service, ModFeedback* feedback_service, Clock clock_source,ModTutorials* tutorial_service,ModDrawing* drawing_service,ModLists* list_service)
        : log(std::move(sink)), guest_log(std::move(guest_sink)), gameplay(game), input(keys), storage(storage_service), settings(settings_service?settings_service:&own_settings), feedback(feedback_service?feedback_service:&own_feedback), tutorials(tutorial_service?tutorial_service:&own_tutorials), drawing(drawing_service?drawing_service:&own_drawing), lists(list_service?list_service:&own_lists), clock(std::move(clock_source)) {
        auto* config = wasm_config_new();
        wasmtime_config_consume_fuel_set(config, true);
        wasmtime_config_max_wasm_stack_set(config, 256 * 1024);
        wasmtime_config_wasm_threads_set(config, false);
        wasmtime_config_wasm_memory64_set(config, false);
        wasmtime_config_wasm_multi_memory_set(config, false);
        wasmtime_config_wasm_gc_set(config, false);
        engine.reset(wasm_engine_new_with_config(config));
        if (!engine) throw std::runtime_error("Cannot create Wasmtime engine");
    }
    std::unique_ptr<Mod> load_one(const std::filesystem::path& directory, Manifest info) {
        auto mod = std::make_unique<Mod>();
        mod->info = std::move(info);
        if(profile_count==profiles.size()) throw std::runtime_error("Profile capacity exceeded");
        auto& profile=profiles[profile_count++];profile.id=mod->info.id;mod->profile=&profile;
        const auto load_started=std::chrono::steady_clock::now();
        // Runs on success and rejection, while the stable profile outlives Mod.
        struct LoadTimer {Profile& profile;std::chrono::steady_clock::time_point started;
            ~LoadTimer() {profile.load_ns=Profile::elapsed(started);}} load_timer{profile,load_started};
        mod->log = &log;
        mod->guest_log = &guest_log;
        mod->gameplay = gameplay;
        mod->input = input;
        mod->storage = storage;
        mod->clock = &clock;
        if(storage && mod->info.has(CRML_CAP_STORAGE)) storage->attach(mod->info.id);
        mod->owner = allocate_owner();
        if(mod->info.has(CRML_CAP_INPUT_ACTIONS)) {
            if(!actions.attach(mod->owner,mod->info.actions))throw std::runtime_error("Input action registry unavailable");
            mod->actions=&actions;
        }
        if(mod->info.has(CRML_CAP_SETTINGS) && settings->attach(mod->owner,mod->info.id,mod->info.metadata))mod->settings=settings;
        if(mod->info.has(CRML_CAP_FEEDBACK) && feedback->attach(mod->owner,mod->info.id,mod->info.metadata))mod->feedback=feedback;
        if(mod->info.has(CRML_CAP_TUTORIALS) && tutorials->attach(mod->owner))mod->tutorials=tutorials;
        if(mod->info.has(CRML_CAP_DRAWING) && drawing->attach(mod->owner,mod->info.id,mod->info.metadata))mod->drawing=drawing;
        if(mod->info.has(CRML_CAP_LISTS) && lists->attach(mod->owner,mod->info.id,mod->info.metadata))mod->lists=lists;
        const auto bytes = read(directory / mod->info.module, max_module);
        if (bytes.size() < 8 || bytes.compare(0, 8, std::string("\0asm\1\0\0\0", 8)) != 0)
            throw std::runtime_error("Only binary core WebAssembly modules are accepted");
        wasmtime_module_t* raw = nullptr;
        const auto compile_started=std::chrono::steady_clock::now();
        auto* compile_error=wasmtime_module_new(engine.get(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &raw);
        profile.compile_ns=Profile::elapsed(compile_started);
        check(compile_error);
        Module module(raw, wasmtime_module_delete);
        mod->store.reset(wasmtime_store_new(engine.get(), nullptr, nullptr));
        wasmtime_store_limiter(mod->store.get(), 16 * 1024 * 1024, 4096, 1, 1, 1);
        Linker linker(wasmtime_linker_new(engine.get()), wasmtime_linker_delete);
        {
            Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new_0_1(wasm_valtype_new_i64()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"clock_ms",8,type.get(),Mod::clock_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_INPUT_ACTIONS)) {
            Owned<wasm_functype_t,wasm_functype_delete> bind(wasm_functype_new_3_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"input_bind",10,bind.get(),Mod::input_memory_callback<true>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> read(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"input_read",10,read.get(),Mod::input_memory_callback<false>,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_TUTORIALS)) {
            wasm_valtype_t* arguments[]{wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()};
            wasm_valtype_t* returns[]{wasm_valtype_new_i64()};wasm_valtype_vec_t params{},results{};
            wasm_valtype_vec_new(&params,6,arguments);wasm_valtype_vec_new(&results,1,returns);
            Owned<wasm_functype_t,wasm_functype_delete> show(wasm_functype_new(&params,&results),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"tutorial_show",13,show.get(),Mod::tutorial_show_callback,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> present(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i64()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"tutorial_present",16,present.get(),Mod::tutorial_present_callback,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> receipt(wasm_functype_new_1_1(wasm_valtype_new_i64(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"tutorial_status",15,receipt.get(),Mod::tutorial_receipt_callback<0>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"tutorial_dismiss",16,receipt.get(),Mod::tutorial_receipt_callback<1>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> available(wasm_functype_new_1_1(wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"tutorial_available",18,available.get(),Mod::tutorial_receipt_callback<2>,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_FEEDBACK)) {
            wasm_valtype_t* arguments[]{wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()};
            wasm_valtype_t* returns[]{wasm_valtype_new_i64()};wasm_valtype_vec_t params{},results{};
            wasm_valtype_vec_new(&params,4,arguments);wasm_valtype_vec_new(&results,1,returns);
            Owned<wasm_functype_t,wasm_functype_delete> show(wasm_functype_new(&params,&results),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"feedback_show",13,show.get(),Mod::feedback_show_callback,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> receipt(wasm_functype_new_1_1(wasm_valtype_new_i64(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"feedback_status",15,receipt.get(),Mod::feedback_receipt_callback<false>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"feedback_dismiss",16,receipt.get(),Mod::feedback_receipt_callback<true>,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_SETTINGS)) {
            Owned<wasm_functype_t,wasm_functype_delete> memory(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"settings_register",17,memory.get(),Mod::settings_memory_callback<true>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"settings_read",13,memory.get(),Mod::settings_memory_callback<false>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> setter(wasm_functype_new_3_1(wasm_valtype_new_i32(),wasm_valtype_new_f64(),wasm_valtype_new_i64(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"settings_set",12,setter.get(),Mod::settings_set_callback,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"settings_text_register",22,memory.get(),Mod::settings_text_callback<0>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> text_read(wasm_functype_new_3_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"settings_text_read",18,text_read.get(),Mod::settings_text_callback<1>,mod.get(),nullptr));
            wasm_valtype_t* arguments[]{wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i64()};
            wasm_valtype_t* returns[]{wasm_valtype_new_i32()};wasm_valtype_vec_t params{},result{};
            wasm_valtype_vec_new(&params,4,arguments);wasm_valtype_vec_new(&result,1,returns);
            Owned<wasm_functype_t,wasm_functype_delete> text_set(wasm_functype_new(&params,&result),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"settings_text_set",17,text_set.get(),Mod::settings_text_callback<2>,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_STORAGE)) {
            Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"storage_read",12,type.get(),Mod::storage_callback<false>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"storage_write",13,type.get(),Mod::storage_callback<true>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> status(wasm_functype_new_0_1(wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"storage_status",14,status.get(),Mod::storage_status_callback,mod.get(),nullptr));
        }
        {
            Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new_0_1(wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"capabilities",12,type.get(),Mod::capabilities_callback,mod.get(),nullptr));
            if(mod->info.has(CRML_CAP_INPUT_ACTIONS))
                check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"input_actions",13,type.get(),Mod::actions_callback,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> cleanup(wasm_functype_new_0_0(),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"release",7,cleanup.get(),Mod::release_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_LISTS)) {
            Owned<wasm_functype_t,wasm_functype_delete> publish(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i64()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"list_publish",12,publish.get(),Mod::list_callback<0>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> next(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"list_next",9,next.get(),Mod::list_callback<1>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> hide(wasm_functype_new_0_1(wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"list_hide",9,hide.get(),Mod::list_callback<2>,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_DRAWING)) {
            Owned<wasm_functype_t,wasm_functype_delete> publish(wasm_functype_new_2_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"drawing_publish",15,publish.get(),Mod::drawing_callback<true>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> hide(wasm_functype_new_0_1(wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"drawing_hide",12,hide.get(),Mod::drawing_callback<false>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_read",8,publish.get(),Mod::map_callback<0>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_projection_read",19,publish.get(),Mod::map_callback<5>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_publish",11,publish.get(),Mod::map_callback<1>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_hide",8,hide.get(),Mod::map_callback<2>,mod.get(),nullptr));
            Owned<wasm_functype_t,wasm_functype_delete> target_read(wasm_functype_new_3_1(wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            Owned<wasm_functype_t,wasm_functype_delete> target_hide(wasm_functype_new_1_1(wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_read_target",15,target_read.get(),Mod::map_callback<3>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_hide_target",15,target_hide.get(),Mod::map_callback<4>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_publish",23,publish.get(),Mod::annotations_callback<0>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_publish_v2",26,publish.get(),Mod::annotations_callback<3>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_next",20,publish.get(),Mod::annotations_callback<1>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_hide",20,hide.get(),Mod::annotations_callback<2>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_publish_v3",26,publish.get(),Mod::annotations_callback<4>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_next_v2",23,publish.get(),Mod::annotations_callback<5>,mod.get(),nullptr));
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"map_annotations_status",22,publish.get(),Mod::annotations_callback<6>,mod.get(),nullptr));
        }
        if (mod->info.has(CRML_CAP_LOG)) {
            wasm_valtype_t* types[]{wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()};
            wasm_valtype_vec_t params{}, results{};
            wasm_valtype_vec_new(&params, 3, types);
            wasm_valtype_vec_new_empty(&results);
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new(&params, &results), wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(), "crml_v1", 7, "log", 3, type.get(), Mod::log_callback, mod.get(), nullptr));
        }
        if (mod->info.has(CRML_CAP_PLAYER_NOCLIP)) {
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new_1_1(wasm_valtype_new_f32(), wasm_valtype_new_i32()), wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(), "crml_v1", 7, "noclip_poll", 11, type.get(), Mod::noclip_callback, mod.get(), nullptr));
        }
        if (mod->info.has(CRML_CAP_INPUT_BUTTONS)) {
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new_0_1(wasm_valtype_new_i32()), wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(), "crml_v1", 7, "input_buttons", 13, type.get(), Mod::input_callback, mod.get(), nullptr));
        }
        if (mod->info.has(CRML_CAP_PLAYER_VISIBILITY)) {
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new_0_1(wasm_valtype_new_i32()), wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(), "crml_v1", 7, "visibility_poll", 15, type.get(), Mod::visibility_callback, mod.get(), nullptr));
        }
        if (mod->info.has(CRML_CAP_PLAYER_VISIBILITY)) {
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32()), wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(), "crml_v1", 7, "visibility_set", 14, type.get(), Mod::visibility_set_callback, mod.get(), nullptr));
        }
        if(mod->info.has(CRML_CAP_PHYSICS_DAMPING)) {
            const char* names[]{"physics_select", "physics_target", "physics_apply", "physics_status", "physics_restore", "physics_select_near"};
            const wasmtime_func_callback_t callbacks[]{Mod::physics_callback<0>,Mod::physics_callback<1>,Mod::physics_callback<2>,Mod::physics_callback<3>,Mod::physics_callback<4>,Mod::physics_callback<5>};
            for(unsigned op=0;op<6;++op) {
                wasm_valtype_vec_t params{},results{};
                if(op==2) {
                    wasm_valtype_t* types[]{wasm_valtype_new_i64(),wasm_valtype_new_f32(),wasm_valtype_new_i32()};
                    wasm_valtype_vec_new(&params,3,types);
                } else if(op==5) {
                    wasm_valtype_t* types[]{wasm_valtype_new_f32(),wasm_valtype_new_f32(),wasm_valtype_new_f32(),wasm_valtype_new_f32()};
                    wasm_valtype_vec_new(&params,4,types);
                } else wasm_valtype_vec_new_empty(&params);
                wasm_valtype_t* output[]{op==1?wasm_valtype_new_i64():wasm_valtype_new_i32()};
                wasm_valtype_vec_new(&results,1,output);
                Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new(&params,&results),wasm_functype_delete);
                check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,names[op],strlen(names[op]),type.get(),callbacks[op],mod.get(),nullptr));
            }
        }
        {
            const char* names[]{"player_read","camera_read","physics_read","ui_read","media_read","motion_read","visibility_read","navigation_read","navigation_read_v2","action_rule_read"};
            const uint32_t bits[]{CRML_CAP_PLAYER_READ,CRML_CAP_CAMERA_READ,CRML_CAP_PHYSICS_DAMPING,CRML_CAP_UI_READ,CRML_CAP_MEDIA_READ,CRML_CAP_PLAYER_MOTION,CRML_CAP_PLAYER_VISIBILITY,CRML_CAP_NAVIGATION_READ,CRML_CAP_NAVIGATION_READ,CRML_CAP_ACTION_RULES};
            const wasmtime_func_callback_t callbacks[]{Mod::state_callback<crml_player_state,0>,
                Mod::state_callback<crml_camera_state,1>,Mod::state_callback<crml_physics_state,2>,
                Mod::state_callback<crml_ui_state,3>,Mod::state_callback<crml_media_state,4>,Mod::state_callback<crml_motion_state,5>,
                Mod::state_callback<crml_visibility_state,6>,Mod::state_callback<crml_navigation_state,7>,
                Mod::state_callback<crml_navigation_state_v2,8>,Mod::state_callback<crml_action_rule_state,9>};
            for(unsigned op=0;op<10;++op) if(mod->info.has(bits[op])) {
                wasm_valtype_vec_t params{},results{};
                if(op==2) {
                    wasm_valtype_t* types[]{wasm_valtype_new_i64(),wasm_valtype_new_i32(),wasm_valtype_new_i32()};
                    wasm_valtype_vec_new(&params,3,types);
                } else {
                    wasm_valtype_t* types[]{wasm_valtype_new_i32(),wasm_valtype_new_i32()};
                    wasm_valtype_vec_new(&params,2,types);
                }
                wasm_valtype_t* output[]{wasm_valtype_new_i32()};wasm_valtype_vec_new(&results,1,output);
                Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new(&params,&results),wasm_functype_delete);
                check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,names[op],strlen(names[op]),type.get(),callbacks[op],mod.get(),nullptr));
            }
        }
        if(mod->info.has(CRML_CAP_ACTION_RULES)) {
            wasm_valtype_vec_t params{},results{};
            wasm_valtype_t* input[]{wasm_valtype_new_i32(),wasm_valtype_new_i32()};
            wasm_valtype_vec_new(&params,2,input);
            wasm_valtype_t* output[]{wasm_valtype_new_i32()};wasm_valtype_vec_new(&results,1,output);
            Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new(&params,&results),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"action_rule_set",15,type.get(),Mod::action_rule_set_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_UI_ACTIVATE)) {
            for(bool tracked:{false,true}) {
                wasm_valtype_t* types[]{wasm_valtype_new_i64(),wasm_valtype_new_i32()};
                wasm_valtype_vec_t params{},results{};
                wasm_valtype_vec_new(&params,2,types);
                wasm_valtype_t* output[]{tracked?wasm_valtype_new_i64():wasm_valtype_new_i32()};wasm_valtype_vec_new(&results,1,output);
                Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new(&params,&results),wasm_functype_delete);
                const char* name=tracked?"ui_action_submit":"ui_activate";
                check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,name,strlen(name),type.get(),
                    tracked?Mod::ui_activate_callback<true>:Mod::ui_activate_callback<false>,mod.get(),nullptr));
            }
            Owned<wasm_functype_t,wasm_functype_delete> status(wasm_functype_new_1_1(wasm_valtype_new_i64(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"ui_action_status",16,status.get(),Mod::ui_action_status_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_UI_PRESENTATION)) {
            wasm_valtype_t* types[]{wasm_valtype_new_i64(),wasm_valtype_new_i32(),wasm_valtype_new_i32(),
                wasm_valtype_new_i32(),wasm_valtype_new_i32(),wasm_valtype_new_i32()};
            wasm_valtype_vec_t params{},results{};
            wasm_valtype_vec_new(&params,6,types);
            wasm_valtype_t* output[]{wasm_valtype_new_i32()};wasm_valtype_vec_new(&results,1,output);
            Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new(&params,&results),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"ui_present",10,type.get(),Mod::ui_present_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_MEDIA_SKIP)) {
            Owned<wasm_functype_t,wasm_functype_delete> type(wasm_functype_new_1_1(wasm_valtype_new_i64(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"media_skip",10,type.get(),Mod::media_skip_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_INPUT_MOTION)) {
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new_0_1(wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"input_motion",12,type.get(),Mod::motion_input_callback,mod.get(),nullptr));
        }
        if(mod->info.has(CRML_CAP_PLAYER_MOTION)) {
            Owned<wasm_functype_t, wasm_functype_delete> camera(wasm_functype_new_1_1(wasm_valtype_new_i32(),wasm_valtype_new_i32()),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"motion_camera",13,camera.get(),Mod::motion_camera_callback,mod.get(),nullptr));
            wasm_valtype_t* types[]{wasm_valtype_new_i32(),wasm_valtype_new_f32(),wasm_valtype_new_f32(),wasm_valtype_new_f32()};
            wasm_valtype_vec_t params{},results{};wasm_valtype_vec_new(&params,4,types);
            wasm_valtype_t* output[]{wasm_valtype_new_i32()};wasm_valtype_vec_new(&results,1,output);
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new(&params,&results),wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(),"crml_v1",7,"motion_set",10,type.get(),Mod::motion_set_callback,mod.get(),nullptr));
        }
        // Fuel and memory limits apply even to the module's start function.
        mod->budget();
        wasm_trap_t* trap = nullptr;
        const auto start_started=std::chrono::steady_clock::now();
        auto* error = wasmtime_linker_instantiate(linker.get(), mod->context(), module.get(), &mod->instance, &trap);
        mod->record(Phase::start,start_started,error || trap);
        check(error, trap);
        wasmtime_func_t version{};
        mod->function("crml_abi_version", {}, {WASM_I32}, version, true);
        mod->function("crml_init", {}, {}, mod->init, true);
        mod->has_tick = mod->function("crml_tick", {WASM_F32}, {}, mod->tick, false);
        mod->has_stop = mod->function("crml_shutdown", {}, {}, mod->stop, false);
        wasmtime_val_t result{};
        mod->call(Phase::abi,version, nullptr, 0, &result, 1);
        if (result.of.i32 != 1) throw std::runtime_error("Unsupported guest ABI");
        mod->call(Phase::init,mod->init);
        log("Loaded " + mod->info.id);
        const auto& metadata=mod->info.metadata;
        if(!metadata.name.empty() || !metadata.version.empty() || !metadata.author.empty())
            log("Package " + mod->info.id + ": name=" + metadata.name + "; version=" + metadata.version + "; author=" + metadata.author);
        profile.state="active";
        return mod;
    }
    void fault(Mod& mod, const std::exception& error) {
        mod.alive = false;
        mod.profile->state="disabled";
        if (mod.gameplay) mod.gameplay->release(mod.owner);
        if (mod.settings) {mod.settings->detach(mod.owner);mod.settings=nullptr;}
        if (mod.feedback) {mod.feedback->detach(mod.owner);mod.feedback=nullptr;}
        if (mod.tutorials) {mod.tutorials->detach(mod.owner);mod.tutorials=nullptr;}
        if (mod.drawing) {mod.drawing->detach(mod.owner);mod.drawing=nullptr;}
        if (mod.lists) {mod.lists->detach(mod.owner);mod.lists=nullptr;}
        if (mod.actions) {mod.actions->detach(mod.owner);mod.actions=nullptr;}
        ++failed;
        log("Disabled " + mod.info.id + ": " + clean(error.what()));
        mod.store.reset();
    }
};
Runtime::Runtime(Log log, Gameplay* gameplay, Input* input, GuestLog guest_log, ModStorage* storage, ModSettings* settings, ModFeedback* feedback, Clock clock,ModTutorials* tutorials,ModDrawing* drawing,ModLists* lists)
    : impl_(std::make_unique<Impl>(std::move(log), gameplay, input, std::move(guest_log), storage, settings, feedback, std::move(clock),tutorials,drawing,lists)) {}
Runtime::~Runtime() = default;
void Runtime::load(const std::filesystem::path& root,const std::function<void(uint32_t)>& prepare) {
    if (impl_->loaded) throw std::runtime_error("Runtime already loaded");
    impl_->loaded = true;
    if (!std::filesystem::is_directory(root)) throw std::runtime_error("Mods directory does not exist");
    if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Mods directory must not be a link");
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        if (paths.size() == 32) throw std::runtime_error("At most 32 mod directories are supported");
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::set<std::string> ids;
    std::vector<std::pair<std::filesystem::path,Manifest>> packages;
    uint32_t requested{};
    for (const auto& path : paths) {
        try {
            if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Mod directory must not be a link");
            auto info = manifest(path / "mod.ini");
            if (!ids.insert(info.id).second) throw std::runtime_error("Duplicate mod id");
            requested|=info.capabilities;
            packages.emplace_back(path,std::move(info));
        } catch (const std::exception& error) {
            ++impl_->failed;
            impl_->log("Rejected " + clean(path.filename().string()) + ": " + clean(error.what()));
        }
    }
    if(prepare) prepare(requested);
    for(auto& [path,info]:packages) {
        try {impl_->mods.push_back(impl_->load_one(path,std::move(info)));}
        catch(const std::exception& error) {
            ++impl_->failed;
            impl_->log("Rejected " + clean(path.filename().string()) + ": " + clean(error.what()));
        }
    }
}
void Runtime::tick(float seconds) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::runtime_error("Invalid tick duration");
    wasmtime_val_t argument{};
    argument.kind = WASMTIME_F32;
    argument.of.f32 = std::min(seconds, 1.0f);
    for (auto& mod : impl_->mods) {
        if (!mod->alive || !mod->has_tick) continue;
        try { mod->call(Impl::Phase::tick,mod->tick, &argument, 1); }
        catch (const std::exception& error) { impl_->fault(*mod, error); }
    }
}
void Runtime::shutdown() {
    for (auto it = impl_->mods.rbegin(); it != impl_->mods.rend(); ++it) {
        auto& mod = **it;
        if (!mod.alive) continue;
        try { if (mod.has_stop) mod.call(Impl::Phase::shutdown,mod.stop); }
        catch (const std::exception& error) { impl_->fault(mod, error); }
        mod.alive = false;
        if(mod.profile->state==std::string_view("active")) mod.profile->state="stopped";
        if (mod.gameplay) mod.gameplay->release(mod.owner);
        if (mod.settings) {mod.settings->detach(mod.owner);mod.settings=nullptr;}
        if (mod.feedback) {mod.feedback->detach(mod.owner);mod.feedback=nullptr;}
        if (mod.tutorials) {mod.tutorials->detach(mod.owner);mod.tutorials=nullptr;}
        if (mod.drawing) {mod.drawing->detach(mod.owner);mod.drawing=nullptr;}
        if (mod.lists) {mod.lists->detach(mod.owner);mod.lists=nullptr;}
        if (mod.actions) {mod.actions->detach(mod.owner);mod.actions=nullptr;}
        mod.store.reset();
    }
}
size_t Runtime::active() const {
    return static_cast<size_t>(std::count_if(impl_->mods.begin(), impl_->mods.end(), [](const auto& m) { return m->alive; }));
}
size_t Runtime::failures() const { return impl_->failed; }
void Runtime::report_metrics() {
    constexpr const char* names[]{"start","abi","init","tick","shutdown"};
    constexpr const char* calls[]{"reads","commands","bindings","storage","settings","feedback","logs","tutorials"};
    for(size_t i=0;i<impl_->profile_count;++i) {
        const auto& profile=impl_->profiles[i];
        impl_->log("Metrics " + profile.id + " state=" + profile.state +
            " load_ns=" + std::to_string(profile.load_ns) + " compile_ns=" + std::to_string(profile.compile_ns));
        for(size_t phase=0;phase<profile.phases.size();++phase) {
            const auto& stats=profile.phases[phase];if(!stats.count) continue;
            std::string line="Metrics " + profile.id + " phase=" + names[phase] +
                " calls="+std::to_string(stats.count)+" failures="+std::to_string(stats.failures)+
                " total_ns="+std::to_string(stats.total_ns)+" max_ns="+std::to_string(stats.max_ns)+
                " fuel_peak="+std::to_string(stats.fuel_peak)+" fuel_errors="+std::to_string(stats.fuel_errors);
            for(size_t j=0;j<stats.calls_peak.size();++j) line+=" "+std::string(calls[j])+"_peak="+std::to_string(stats.calls_peak[j]);
            impl_->log(line);
        }
    }
}
}
