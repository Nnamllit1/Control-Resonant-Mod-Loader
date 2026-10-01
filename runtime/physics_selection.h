#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace crml::physics {
// Values and identity tokens only. Each batch receives a fresh callback scope.
struct SelectionScope {
    uint64_t world{}, owner{}, player{}, retirement{};
    uint32_t slots{};
    float position[3]{};
};
struct SelectionCandidate { uint64_t entity{}, body{}, actor{}; float distance_squared{}; };
// World-aligned offset from the player position captured when the search starts.
// The guest chooses the region; identity/eligibility and work bounds stay native.
struct SelectionQuery {
    float offset[3]{};
    float radius{2.f};
    bool valid() const noexcept {
        return std::isfinite(offset[0]) && std::isfinite(offset[1]) && std::isfinite(offset[2])
            && std::hypot(offset[0],offset[1],offset[2])<=20.f
            && std::isfinite(radius) && radius>0 && radius<=20.f;
    }
    float distance_squared(const float (&anchor)[3],const float (&position)[3]) const noexcept {
        float result{};
        for(unsigned i=0;i<3;++i) {const auto delta=position[i]-anchor[i]-offset[i];result+=delta*delta;}
        return result;
    }
};
enum class SelectionResult { pending, selected, none, ambiguous, changed, invalid, timeout };
class SelectionSearch {
public:
    SelectionResult begin(const SelectionScope& scope, uint64_t now,SelectionQuery query={}) noexcept {
        query_=query;
        scope_=scope; started_=last_=now; next_=matches_=0; candidate_={};
        nearest_=second_=std::numeric_limits<float>::infinity();
        return result_=valid(scope) && query.valid()?SelectionResult::pending:SelectionResult::invalid;
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
                if(!std::isfinite(candidate.distance_squared) || candidate.distance_squared<0 || candidate.distance_squared>query_.radius*query_.radius)
                    return result_=SelectionResult::invalid;
                ++matches_;
                if(candidate.distance_squared<nearest_) {
                    second_=nearest_;nearest_=candidate.distance_squared;candidate_=candidate;
                } else if(candidate.distance_squared<second_) second_=candidate.distance_squared;
            }
            if((work&63)==63 && yield()) break;
        }
        // Search the whole table: later entries can be closer than either of
        // the first two. Near ties need a more deliberate player position.
        if(next_==scope.slots) result_=!matches_?SelectionResult::none:
            std::sqrt(second_)-std::sqrt(nearest_)<.1f?SelectionResult::ambiguous:SelectionResult::selected;
        return result_;
    }
    bool pending() const noexcept { return result_==SelectionResult::pending; }
    void cancel() noexcept { result_=SelectionResult::changed; }
    uint32_t scanned() const noexcept { return next_; }
    uint32_t matches() const noexcept { return matches_; }
    float nearest_distance() const noexcept { return matches_?std::sqrt(nearest_):-1.f; }
    float second_distance() const noexcept { return matches_>1?std::sqrt(second_):-1.f; }
    const SelectionCandidate& candidate() const noexcept { return candidate_; }
    const SelectionScope& scope() const noexcept { return scope_; }
    const SelectionQuery& query() const noexcept { return query_; }
private:
    static bool valid(const SelectionScope& s) noexcept {
        return s.world && s.owner && s.player && s.slots && s.slots<=(1u<<20)
            && std::isfinite(s.position[0]) && std::isfinite(s.position[1]) && std::isfinite(s.position[2]);
    }
    SelectionScope scope_{};
    SelectionQuery query_{};
    SelectionCandidate candidate_{};
    uint64_t started_{},last_{};
    uint32_t next_{},matches_{};
    float nearest_{},second_{};
    SelectionResult result_{SelectionResult::invalid};
};
}
