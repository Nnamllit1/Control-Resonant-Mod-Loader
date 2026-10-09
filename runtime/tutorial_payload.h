#pragma once
#include "tutorial_panel.h"
#include <string_view>

namespace crml::tutorial {
struct PageVector {void* data{};uint32_t count{},capacity{};};
struct PageSource {uint64_t count{};const void* data{};};
// Calls use the game's allocator and string ABI. A false result after a native
// fault quarantines the partial allocation: never attempt speculative cleanup.
struct PayloadNative {
    uintptr_t image{};
    void* (*assign)(void*,const NativeStringView*){};
    void (*destroy_string)(void*){};
    PageVector* (*build)(PageVector*,const PageSource*){};
    void (*destroy_pages)(PageVector*){};
    static bool bind(uintptr_t,PayloadNative&) noexcept;
    bool make(std::string_view title,std::string_view body,PageVector&) const noexcept;
    bool destroy(PageVector&) const noexcept;
};
}
