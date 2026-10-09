#pragma once
#include "../sdk/include/crml_settings.h"
#include "mod_metadata.h"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace crml {
class ModSettings {
public:
    struct Item {crml_setting_definition definition;crml_setting_value state;uint32_t max_bytes{};std::string text;};
    struct Group {uint64_t owner;std::string id;std::vector<Item> items;ModMetadata metadata;};
    ModSettings();
    ~ModSettings();
    ModSettings(const ModSettings&)=delete;
    ModSettings& operator=(const ModSettings&)=delete;
    bool attach(uint64_t owner,std::string_view id,const ModMetadata& metadata={});
    void detach(uint64_t owner);
    int define(uint64_t owner,const crml_setting_definition& definition);
    int read(uint64_t owner,std::span<crml_setting_value> output);
    int set(uint64_t owner,uint32_t handle,double value,uint64_t expected_revision=0);
    int define_text(uint64_t owner,const crml_text_setting_definition& definition);
    int read_text(uint64_t owner,uint32_t handle,crml_text_setting_value& output);
    int set_text(uint64_t owner,uint32_t handle,std::string_view value,uint64_t expected_revision=0);
    std::vector<Group> snapshot();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Used by process-pinned renderer hooks; no pointer to a Runtime-owned service.
ModSettings& process_settings();
}
