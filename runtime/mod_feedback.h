#pragma once
#include "mod_metadata.h"
#include "../sdk/include/crml_feedback.h"
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace crml {
inline constexpr std::string_view feedback_prefix="coui://base/crml/feedback/v1/";
class ModFeedback {
public:
    using Clock=std::function<uint64_t()>;
    struct Message {uint64_t ticket;std::string id,name,text;uint32_t severity,remaining_ms;};
    explicit ModFeedback(Clock clock={});
    ~ModFeedback();
    ModFeedback(const ModFeedback&)=delete;
    ModFeedback& operator=(const ModFeedback&)=delete;
    bool attach(uint64_t owner,std::string_view id,const ModMetadata& metadata={});
    void detach(uint64_t owner);
    void cancel(uint64_t owner);
    bool available();
    void enable_renderer(bool enabled);
    uint64_t open_page();
    int64_t show(uint64_t owner,std::string_view text,uint32_t severity,uint32_t duration_ms);
    int status(uint64_t owner,uint64_t ticket);
    int dismiss(uint64_t owner,uint64_t ticket);
    // Trusted renderer boundary. Acknowledges only tickets offered in its last
    // response; never grants a guest access to another owner's receipts.
    int poll(uint64_t page,uint64_t sequence,std::span<const uint64_t> acknowledgements,std::vector<Message>& output);
    int exchange(std::string_view url,std::string& output) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
ModFeedback& process_feedback();
}
