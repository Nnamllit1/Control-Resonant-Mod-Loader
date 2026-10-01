#pragma once
#include "runtime.h"
#include <Windows.h>
#include <atomic>
#include <string_view>

namespace crml::media {
class Service final : public Gameplay {
public:
    void enable(bool enabled) noexcept;
    uint32_t capabilities() const noexcept override;
    int noclip_poll(uint64_t,float) noexcept override {return -1;}
    int media_read(crml_media_state& out) noexcept override;
    int media_skip(uint64_t owner,uint64_t generation) noexcept override;
    void release(uint64_t owner) noexcept override;
    int read_at(crml_media_state& out,uint64_t now) noexcept;
    int request_at(uint64_t owner,uint64_t generation,uint64_t now) noexcept;
    // Called only by an engine adapter on its owning thread. True means a
    // pending request was consumed at a reviewed native skip point.
    bool observe_at(uintptr_t identity,bool active,bool ready,uint32_t elapsed,
                    std::string_view name,uint32_t name_source,uint64_t now,bool consume) noexcept;
    uint64_t observations() const noexcept {return observations_.load();}
    uint64_t submissions() const noexcept {return submissions_.load();}
    uint64_t skips() const noexcept {return skips_.load();}
private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic<bool> enabled_{};
    std::atomic<uint64_t> observations_{},submissions_{},skips_{};
    crml_media_state state_{};
    uintptr_t identity_{},retired_{};
    uint64_t generation_{},sampled_at_{},owner_{},requested_at_{};
};
Service& process_service();
// Version-gated native boot-media adapter. Names come from reviewed engine
// resource metadata, not a CRML screen-name table.
std::string start();
void stop() noexcept;
}
