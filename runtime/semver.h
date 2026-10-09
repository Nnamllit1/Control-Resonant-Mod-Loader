#pragma once
#include <array>
#include <optional>
#include <string_view>

namespace crml {
// Views refer to the caller's string. Bounded parsing/comparison needs no heap
// allocation and compares numeric identifiers without integer overflow.
struct Semver {
    std::array<std::string_view, 3> core;
    std::string_view prerelease;

    static constexpr bool numeric(std::string_view s) noexcept {
        return !s.empty() && s.find_first_not_of("0123456789") == s.npos;
    }
    static constexpr std::string_view take(std::string_view& s) noexcept {
        const auto dot = s.find('.');
        const auto part = s.substr(0, dot);
        s = dot == s.npos ? std::string_view{} : s.substr(dot + 1);
        return part;
    }
    static constexpr bool identifiers(std::string_view s, bool prerelease) noexcept {
        if (s.empty() || s.back() == '.') return false;
        while (!s.empty()) {
            const auto part = take(s);
            if (part.empty() || part.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") != part.npos)
                return false;
            if (prerelease && numeric(part) && part.size() > 1 && part.front() == '0') return false;
        }
        return true;
    }
    static constexpr std::optional<Semver> parse(std::string_view s) noexcept {
        if (s.empty() || s.size() > 96) return {};
        const auto plus = s.find('+');
        if (plus != s.npos) {
            if (!identifiers(s.substr(plus + 1), false)) return {};
            s = s.substr(0, plus);
        }
        Semver result{};
        const auto dash = s.find('-');
        if (dash != s.npos) {
            result.prerelease = s.substr(dash + 1);
            if (!identifiers(result.prerelease, true)) return {};
            s = s.substr(0, dash);
        }
        for (size_t i = 0; i < 3; ++i) {
            const auto dot = s.find('.');
            if ((i < 2) != (dot != s.npos)) return {};
            const auto part = take(s);
            if (!numeric(part) || (part.size() > 1 && part.front() == '0')) return {};
            result.core[i] = part;
        }
        return result;
    }
    static int number_compare(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        return a == b ? 0 : a < b ? -1 : 1;
    }
    int compare(const Semver& other) const noexcept {
        for (size_t i = 0; i < 3; ++i)
            if (const auto order = number_compare(core[i], other.core[i])) return order;
        auto a = prerelease, b = other.prerelease;
        if (a.empty() || b.empty()) return a == b ? 0 : a.empty() ? 1 : -1;
        while (!a.empty() && !b.empty()) {
            const auto x = take(a), y = take(b);
            if (x == y) continue;
            const bool nx = numeric(x), ny = numeric(y);
            if (nx && ny) return number_compare(x, y);
            if (nx != ny) return nx ? -1 : 1;
            return x < y ? -1 : 1;
        }
        return a == b ? 0 : a.empty() ? -1 : 1;
    }
};
}
