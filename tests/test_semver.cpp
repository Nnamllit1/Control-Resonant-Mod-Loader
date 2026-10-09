#include "semver.h"
#include <iostream>
#include <stdexcept>
#include <string>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        // SemVer 2.0.0 precedence examples plus alpha and overflow boundaries.
        constexpr std::string_view ordered[]{
            "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
            "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "2.0.0"};
        for (size_t i = 0; i < std::size(ordered); ++i) {
            const auto a = crml::Semver::parse(ordered[i]);
            require(bool(a), "valid version rejected");
            for (size_t j = 0; j < std::size(ordered); ++j) {
                const auto b = crml::Semver::parse(ordered[j]);
                require(a->compare(*b) == (i == j ? 0 : i < j ? -1 : 1), "precedence ordering");
            }
        }
        for (const auto invalid : {"", "v1.0.0", "1.0", "1.0.0.0", "01.0.0", "1.00.0", "1.0.00",
                "1.0.0-", "1.0.0-alpha..1", "1.0.0-alpha.", "1.0.0-01", "1.0.0+", "1.0.0+a..b",
                "1.0.0+a+z", "1.0.0+a.", "1.0.0 a", "1.0.0-ä", "1.0.0-α", "1.0.0-1_2"})
            require(!crml::Semver::parse(invalid), "invalid version accepted");
        require(!crml::Semver::parse(std::string(97, '9')), "length ceiling");
        const auto equal = crml::Semver::parse("1.0.0-alpha+001");
        require(equal->compare(*crml::Semver::parse("1.0.0-alpha+other")) == 0, "metadata does not change precedence");
        require(crml::Semver::parse("0.1.0-alpha.4.3")->compare(*crml::Semver::parse("0.1.0-alpha.4.3.dev.0")) < 0,
                "development follows published baseline");
        require(crml::Semver::parse("0.1.0-alpha.4.3.dev.0")->compare(*crml::Semver::parse("0.1.0-alpha.4.4")) < 0,
                "development precedes next release");
        require(crml::Semver::parse("1.0.0-alpha.9")->compare(*crml::Semver::parse("1.0.0-alpha.10")) < 0,
                "numeric prerelease ordering");
        require(crml::Semver::parse("99999999999999999999.0.0")->compare(*crml::Semver::parse("100000000000000000000.0.0")) < 0,
                "core numbers do not overflow");
        require(crml::Semver::parse("1.0.0-99999999999999999999")->compare(*crml::Semver::parse("1.0.0-100000000000000000000")) < 0,
                "prerelease numbers do not overflow");
        std::cout << "Semantic version grammar and precedence checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
