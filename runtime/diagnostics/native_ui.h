#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace crml::ui {class Service;}
namespace crml::native_ui {
// Development-only proof using the game's existing UI document and resource
// response. No Wasm or engine pointers are exposed to mods.
std::string start(const std::filesystem::path& root,ui::Service* bridge=nullptr,bool settings=false,bool feedback=false,bool tutorials=false,bool drawing=false,bool lists=false);
bool active() noexcept;
void poll();
void stop() noexcept;

inline constexpr size_t max_payload=128*1024;
inline constexpr size_t max_document=8*1024*1024;
inline constexpr std::string_view route="coui://base/uiresources/game/ui/ui.html";
bool target_url(std::string_view url) noexcept;
bool bind_tutorial_layout(std::string_view document,std::string& output);
bool insert_panel(std::string_view document,std::string_view payload,std::string& output);

}
