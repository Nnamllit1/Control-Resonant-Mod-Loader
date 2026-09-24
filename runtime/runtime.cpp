#include "runtime.h"
#include <wasmtime.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace crml {
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
struct Manifest { std::string id, module; bool log = false; };
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
        if (key != "id" && key != "abi" && key != "module" && key != "capabilities")
            throw std::runtime_error("Unknown manifest field: " + key);
        if (!fields.emplace(key, value).second) throw std::runtime_error("Duplicate manifest field");
    }
    Manifest m{fields["id"], fields["module"], fields["capabilities"] == "log"};
    if (m.id.empty() || m.id.size() > 64 || m.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::runtime_error("Invalid mod id");
    if (fields["abi"] != "1") throw std::runtime_error("Unsupported manifest ABI");
    if (!fields["capabilities"].empty() && !m.log) throw std::runtime_error("Unsupported capability");
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
    struct Mod {
        Manifest info;
        Log* log;
        size_t log_bytes = 0;
        size_t log_calls = 0;
        bool alive = true;
        Store store{nullptr, wasmtime_store_delete};
        wasmtime_instance_t instance{};
        wasmtime_func_t init{}, tick{}, stop{};
        bool has_tick = false, has_stop = false;
        wasmtime_context_t* context() { return wasmtime_store_context(store.get()); }
        void budget() {
            log_bytes = 0;
            log_calls = 0;
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
        void call(wasmtime_func_t& fn, const wasmtime_val_t* args = nullptr, size_t nargs = 0,
                  wasmtime_val_t* results = nullptr, size_t nresults = 0) {
            budget();
            wasm_trap_t* trap = nullptr;
            auto* error = wasmtime_func_call(context(), &fn, args, nargs, results, nresults, &trap);
            check(error, trap);
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
                (*mod.log)("[" + mod.info.id + "] " + clean(text));
                return nullptr;
            } catch (...) { return fail("Host log failed"); }
        }
    };
    Log log;
    Engine engine{nullptr, wasm_engine_delete};
    std::vector<std::unique_ptr<Mod>> mods;
    size_t failed = 0;
    bool loaded = false;
    explicit Impl(Log sink) : log(std::move(sink)) {
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
        mod->log = &log;
        const auto bytes = read(directory / mod->info.module, max_module);
        if (bytes.size() < 8 || bytes.compare(0, 8, std::string("\0asm\1\0\0\0", 8)) != 0)
            throw std::runtime_error("Only binary core WebAssembly modules are accepted");
        wasmtime_module_t* raw = nullptr;
        check(wasmtime_module_new(engine.get(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &raw));
        Module module(raw, wasmtime_module_delete);
        mod->store.reset(wasmtime_store_new(engine.get(), nullptr, nullptr));
        wasmtime_store_limiter(mod->store.get(), 16 * 1024 * 1024, 4096, 1, 1, 1);
        Linker linker(wasmtime_linker_new(engine.get()), wasmtime_linker_delete);
        if (mod->info.log) {
            wasm_valtype_t* types[]{wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()};
            wasm_valtype_vec_t params{}, results{};
            wasm_valtype_vec_new(&params, 3, types);
            wasm_valtype_vec_new_empty(&results);
            Owned<wasm_functype_t, wasm_functype_delete> type(wasm_functype_new(&params, &results), wasm_functype_delete);
            check(wasmtime_linker_define_func(linker.get(), "crml_v1", 7, "log", 3, type.get(), Mod::log_callback, mod.get(), nullptr));
        }
        // Fuel and memory limits apply even to the module's start function.
        mod->budget();
        wasm_trap_t* trap = nullptr;
        auto* error = wasmtime_linker_instantiate(linker.get(), mod->context(), module.get(), &mod->instance, &trap);
        check(error, trap);
        wasmtime_func_t version{};
        mod->function("crml_abi_version", {}, {WASM_I32}, version, true);
        mod->function("crml_init", {}, {}, mod->init, true);
        mod->has_tick = mod->function("crml_tick", {WASM_F32}, {}, mod->tick, false);
        mod->has_stop = mod->function("crml_shutdown", {}, {}, mod->stop, false);
        wasmtime_val_t result{};
        mod->call(version, nullptr, 0, &result, 1);
        if (result.of.i32 != 1) throw std::runtime_error("Unsupported guest ABI");
        mod->call(mod->init);
        log("Loaded " + mod->info.id);
        return mod;
    }
    void fault(Mod& mod, const std::exception& error) {
        mod.alive = false;
        ++failed;
        log("Disabled " + mod.info.id + ": " + clean(error.what()));
        mod.store.reset();
    }
};
Runtime::Runtime(Log log) : impl_(std::make_unique<Impl>(std::move(log))) {}
Runtime::~Runtime() = default;
void Runtime::load(const std::filesystem::path& root) {
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
    for (const auto& path : paths) {
        try {
            if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("Mod directory must not be a link");
            auto info = manifest(path / "mod.ini");
            if (!ids.insert(info.id).second) throw std::runtime_error("Duplicate mod id");
            impl_->mods.push_back(impl_->load_one(path, std::move(info)));
        } catch (const std::exception& error) {
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
        try { mod->call(mod->tick, &argument, 1); }
        catch (const std::exception& error) { impl_->fault(*mod, error); }
    }
}
void Runtime::shutdown() {
    for (auto it = impl_->mods.rbegin(); it != impl_->mods.rend(); ++it) {
        auto& mod = **it;
        if (!mod.alive) continue;
        try { if (mod.has_stop) mod.call(mod.stop); }
        catch (const std::exception& error) { impl_->fault(mod, error); }
        mod.alive = false;
        mod.store.reset();
    }
}
size_t Runtime::active() const {
    return static_cast<size_t>(std::count_if(impl_->mods.begin(), impl_->mods.end(), [](const auto& m) { return m->alive; }));
}
size_t Runtime::failures() const { return impl_->failed; }
}
