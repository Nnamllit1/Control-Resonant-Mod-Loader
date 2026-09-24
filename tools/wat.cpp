#include <wasmtime.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::cerr << "Usage: crml_wat input.wat output.wasm\n"; return 2; }
    std::ifstream file(std::filesystem::path(argv[1]), std::ios::binary);
    if (!file) { std::cerr << "Cannot read input\n"; return 1; }
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    wasm_byte_vec_t bytes{};
    if (auto* error = wasmtime_wat2wasm(text.data(), text.size(), &bytes)) {
        wasm_byte_vec_t message{};
        wasmtime_error_message(error, &message);
        std::cerr.write(message.data, message.size);
        wasm_byte_vec_delete(&message);
        wasmtime_error_delete(error);
        return 1;
    }
    std::ofstream out(std::filesystem::path(argv[2]), std::ios::binary);
    out.write(bytes.data, bytes.size);
    wasm_byte_vec_delete(&bytes);
    return out ? 0 : 1;
}
