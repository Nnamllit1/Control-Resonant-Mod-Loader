#pragma once
#include "mod_metadata.h"
#include "../sdk/include/crml_lists.h"
#include <functional>
#include <memory>
#include <vector>

namespace crml {
class ModLists {
public:
    using Clock=std::function<uint64_t()>;
    struct Group {uint64_t owner,revision;std::string id;ModMetadata metadata;crml_list_page page;};
    explicit ModLists(Clock clock={});
    ~ModLists();
    ModLists(const ModLists&)=delete;
    ModLists& operator=(const ModLists&)=delete;
    bool attach(uint64_t owner,std::string_view id,const ModMetadata& metadata={});
    void detach(uint64_t owner);
    void cancel(uint64_t owner);
    int hide(uint64_t owner); // 1 cleared, 0 already clear, -1 unknown owner.
    void clear(); // Explicitly clear all views and pending actions.
    void enable_renderer(bool enabled);
    bool available();
    // Positive host revision; -1 unavailable/owner, -2 capacity (reserved),
    // -3 invalid, -4 rate (100 ms/owner), -5 exhaustion. Not presentation proof.
    int64_t publish(uint64_t owner,const crml_list_page& page);
    int next(uint64_t owner,crml_list_event& output); // 1 event,0 none,-1 unavailable.
    // Transport authenticates page nonce/request replay before entering here.
    // 200 accepted,400 invalid,404 owner/row absent,409 stale/disabled,
    // 429 queue full,503 renderer unavailable,507 sequence exhausted.
    int activate(uint64_t owner,uint64_t revision,uint64_t row_id);
    std::vector<Group> snapshot();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
ModLists& process_lists();
}
