#pragma once
#include "runtime.h"
#include <Windows.h>
#include <atomic>
#include <string>
#include <string_view>
#include <array>

namespace crml::ui {
inline constexpr std::string_view poll_prefix="coui://base/crml/ui/v1/";
inline constexpr std::string_view result_poll_prefix="coui://base/crml/ui/v2/";
// Bounded data exchange with the runtime-owned renderer bootstrap. Guests never
// receive document contents, native pointers, URLs or JavaScript execution.
class Service final : public Gameplay {
public:
    void enable(bool value) noexcept;
    uint64_t open_page() noexcept;
    uint32_t capabilities() const noexcept override;
    int ui_read(crml_ui_state& out) noexcept override;
    int ui_activate(uint64_t owner,uint64_t generation,uint32_t action) noexcept override;
    int64_t ui_action_submit(uint64_t owner,uint64_t generation,uint32_t action) noexcept override;
    int ui_action_status(uint64_t owner,uint64_t ticket) noexcept override;
    int ui_present(uint64_t owner,uint64_t generation,uint32_t kind,std::string_view name,bool hidden,uint32_t duration) noexcept override;
    int noclip_poll(uint64_t,float) noexcept override {return -1;}
    void release(uint64_t owner) noexcept override;
    int read_at(crml_ui_state& out,uint64_t now) noexcept;
    int activate_at(uint64_t owner,uint64_t generation,uint32_t action,uint64_t now) noexcept;
    int64_t submit_at(uint64_t owner,uint64_t generation,uint32_t action,uint64_t now) noexcept;
    int status_at(uint64_t owner,uint64_t ticket,uint64_t now) noexcept;
    int present_at(uint64_t owner,uint64_t generation,uint32_t kind,std::string_view name,bool hidden,uint32_t duration,uint64_t now) noexcept;
    // 0: not this endpoint; HTTP status otherwise. now is monotonic milliseconds.
    int exchange(std::string_view url,uint64_t now,std::string& response) noexcept;
    uint64_t polls() const noexcept {return polls_.load();}
    uint64_t submissions() const noexcept {return submissions_.load();}
    uint64_t acknowledgements() const noexcept {return acknowledgements_.load();}
private:
    struct Command {uint64_t owner{},generation{},queued_at{};uint32_t id{},screen{},action{};bool delivered{};};
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic<bool> enabled_{};
    std::atomic<uint64_t> polls_{},submissions_{},acknowledgements_{};
    uint64_t page_{},generation_{},sampled_at_{};
    uint32_t sequence_{},screen_{},actions_{},next_command_{},last_ack_{},last_delivery_{};
    bool sampled_{};
    Command command_{};
    // Most recent 64 accepted tracked commands, across owners. IDs never wrap.
    struct Receipt {uint64_t owner{},page{};uint32_t id{};int status{};};
    std::array<Receipt,64> receipts_{};
    size_t next_receipt_{};
    struct Lease {uint64_t owner{},issued{},deadline{};uint32_t kind{},screen{};char name[65]{};};
    std::array<Lease,8> leases_{};
    void expire(uint64_t now) noexcept;
    int64_t queue_locked(uint64_t owner,uint64_t generation,uint32_t action,uint64_t now,bool tracked) noexcept;
    void finish_command(int undelivered) noexcept;
    Receipt* receipt(uint32_t id) noexcept;
};
// Resource hooks can outlive the bootstrap thread; their provider is pinned.
Service& process_service();
}
