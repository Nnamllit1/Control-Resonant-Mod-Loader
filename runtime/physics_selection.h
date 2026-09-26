#pragma once
#include <cmath>
#include <cstdint>

namespace crml::physics {
// Values and identity tokens only. Each batch receives a fresh callback scope.
struct SelectionScope {
    uint64_t world{}, owner{}, player{}, retirement{};
    uint32_t slots{};
    float position[3]{};
};
struct SelectionCandidate { uint64_t entity{}, body{}, actor{}; };
enum class SelectionResult { pending, selected, none, ambiguous, changed, invalid, timeout };
class SelectionSearch {
public:
    SelectionResult begin(const SelectionScope& scope, uint64_t now) noexcept {
        scope_=scope; started_=last_=now; next_=matches_=0; candidate_={};
        return result_=valid(scope)?SelectionResult::pending:SelectionResult::invalid;
    }
    // visit resolves an eligible nearby candidate from current engine state.
    // yield bounds elapsed work in addition to the hard per-callback slot cap.
    template<class Visit,class Yield>
    SelectionResult step(const SelectionScope& scope,uint64_t now,Visit visit,Yield yield) noexcept {
        if(!pending()) return result_;
        if(now<last_ || now-started_>=15000) return result_=SelectionResult::timeout;
        last_=now;
        float distance{};
        for(unsigned i=0;i<3;++i) distance+=(scope.position[i]-scope_.position[i])*(scope.position[i]-scope_.position[i]);
        if(!valid(scope) || scope.world!=scope_.world || scope.owner!=scope_.owner || scope.player!=scope_.player
           || scope.retirement!=scope_.retirement || scope.slots!=scope_.slots || distance>.0625f)
            return result_=SelectionResult::changed;
        for(unsigned work=0;next_<scope.slots && work<4096;++work) {
            SelectionCandidate candidate{};
            if(visit(next_++,candidate)) {
                candidate_=candidate;
                if(++matches_>1) return result_=SelectionResult::ambiguous;
            }
            if((work&63)==63 && yield()) break;
        }
        if(next_==scope.slots) result_=matches_?SelectionResult::selected:SelectionResult::none;
        return result_;
    }
    bool pending() const noexcept { return result_==SelectionResult::pending; }
    void cancel() noexcept { result_=SelectionResult::changed; }
    uint32_t scanned() const noexcept { return next_; }
    const SelectionCandidate& candidate() const noexcept { return candidate_; }
    const SelectionScope& scope() const noexcept { return scope_; }
private:
    static bool valid(const SelectionScope& s) noexcept {
        return s.world && s.owner && s.player && s.slots && s.slots<=(1u<<20)
            && std::isfinite(s.position[0]) && std::isfinite(s.position[1]) && std::isfinite(s.position[2]);
    }
    SelectionScope scope_{};
    SelectionCandidate candidate_{};
    uint64_t started_{},last_{};
    uint32_t next_{},matches_{};
    SelectionResult result_{SelectionResult::invalid};
};
}
