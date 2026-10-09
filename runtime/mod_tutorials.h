#pragma once
#include "../sdk/include/crml_tutorial.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace crml {
// Copied guest data. Native code receives no guest pointer or borrowed memory.
class ModTutorials {
public:
    struct Request {uint64_t ticket{},owner{};uint32_t kind{},duration_ms{};std::string title,body,image_url;crml_tutorial_image_layout image{};uint8_t options{};};
    ModTutorials();
    ~ModTutorials();
    ModTutorials(const ModTutorials&)=delete;
    ModTutorials& operator=(const ModTutorials&)=delete;
    bool attach(uint64_t owner);
    void detach(uint64_t owner);
    void cancel(uint64_t owner);
    int64_t show(uint64_t owner,uint32_t kind,std::string_view title,std::string_view body,uint32_t duration_ms,
        std::string_view image_url={},crml_tutorial_image_layout image={},uint32_t options=UINT32_MAX);
    int64_t present(uint64_t owner,const crml_tutorial_page& page);
    int status(uint64_t owner,uint64_t ticket);
    int dismiss(uint64_t owner,uint64_t ticket);
    bool available(uint32_t kind);
    // Native/simulator boundary. Disable requests cancellation; it cannot
    // acknowledge retirement of a presentation that native code still owns.
    void enable(uint32_t kind,bool enabled);
    bool take(uint32_t kind,Request& out);
    // Hint and prompt share one native dynamic-toast state; claim their oldest
    // queued request atomically so one kind cannot overtake the other.
    bool take_dynamic(Request& out);
    bool cancelled(uint64_t ticket);
    // Terminal reports must follow actual native cleanup. PRESENTED is an
    // observation, not queue acceptance. CANCELLED cannot be inferred from
    // a close request alone. Late reports cannot resurrect terminal tickets.
    void report(uint64_t ticket,int status);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
ModTutorials& process_tutorials();
// Stock tutorial body uses an HTML sink. Native adapters must escape plain
// guest text before storing the localization fallback; no arbitrary markup.
bool tutorial_image_url_valid(std::string_view url);
std::string tutorial_body_markup(std::string_view text,std::string_view image_url={},crml_tutorial_image_layout image={});
}
