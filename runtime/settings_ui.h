#pragma once
#include "mod_settings.h"
#include <mutex>

namespace crml {
class ModLists;
inline constexpr std::string_view settings_prefix="coui://base/crml/settings/v1/";
// Independent renderer protocol; no startup-screen command/lease state shared.
class SettingsUi {
public:
    explicit SettingsUi(ModSettings& settings,ModLists* lists=nullptr):settings_(settings),lists_(lists){}
    void enable(bool enabled);
    uint64_t open_page();
    int exchange(std::string_view url,std::string& response) noexcept;
private:
    ModSettings& settings_;
    ModLists* lists_{};
    std::mutex mutex_;
    bool enabled_{};
    uint64_t page_{},sequence_{};
};
SettingsUi& process_settings_ui();
}
