#include "noclip.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace crml::probe {
bool Flight::step(const Sample& sample, uint64_t current_world, uint64_t now, bool focused,
                  Direction input, std::array<float, 3>& target) noexcept {
    if (!enabled) return false;
    if (!focused || now < lease || now - lease > 500 || sample.entity != entity || current_world != world ||
        sample.disabled || sample.keyframed[0] || sample.keyframed[1] ||
        (last_step && (now < last_step || now - last_step > 250)) || !std::isfinite(speed) || speed < .25f || speed > 20) {
        reset(); return false;
    }
    const float dt = last_step ? std::min(float(now - last_step) / 1000.f, .05f) : 0.f;
    last_step = now;
    const float length = std::sqrt(input.x*input.x + input.y*input.y + input.z*input.z);
    if (!std::isfinite(length)) { reset(); return false; }
    const float distance = speed * (input.fast ? 3.f : 1.f) * dt / std::max(length, 1.f);
    for (int i=0; i<3; ++i) {
        const float axis = i==0 ? input.x : i==1 ? input.y : input.z;
        target[i] = sample.position[i] + axis * distance;
        if (!std::isfinite(target[i])) { reset(); return false; }
    }
    return true;
}

bool Override::prepare(const void* original, const Sample& sample, const std::array<float,3>& target) noexcept {
    __try {
        std::memcpy(view.data(), original, sizeof(view));
        if (view[18] != sample.row || sample.row >= 16384) return false;
        std::memcpy(transform.data(), reinterpret_cast<void*>(view[11] + sample.row*32ull), 32);
        for (int i=0; i<3; ++i) transform[4+i] = target[i];
        // These are integer ABI anchors, not C++ array pointers to dereference here.
        // The verified native routine adds row*stride before reading each component.
        view[11] = reinterpret_cast<uintptr_t>(transform.data()) - sample.row*32ull;
        view[7] = reinterpret_cast<uintptr_t>(keyframed.data()) - sample.row*2ull;
        return true;
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
}
