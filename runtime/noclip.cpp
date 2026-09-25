#include "noclip.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace crml::probe {
Direction camera_relative(Direction input, const CameraBasis& camera) noexcept {
    // Keep WASD horizontal; Space/Ctrl controls altitude independently of camera pitch.
    if(!camera.valid) return {0,input.y,0,input.fast};
    const float length=std::hypot(camera.right[0],camera.right[2]);
    if(!std::isfinite(length) || length<.01f) return {0,input.y,0,input.fast};
    const float rx=camera.right[0]/length, rz=camera.right[2]/length;
    return {input.x*rx-input.z*rz,input.y,input.x*rz+input.z*rx,input.fast};
}
bool Flight::step(const Sample& sample, uint64_t current_world, uint64_t now, bool focused,
                  Direction input, std::array<float, 3>& target) noexcept {
    if (!enabled) return false;
    if (!focused || now < lease || now - lease > 500 || sample.entity != entity || current_world != world ||
        sample.disabled || sample.teleported || sample.keyframed[0] || sample.keyframed[1] ||
        (last_step && (now < last_step || now - last_step > 250)) || !std::isfinite(speed) || speed < .25f || speed > 20) {
        reset(); return false;
    }
    const float dt = last_step ? std::min(float(now - last_step) / 1000.f, .05f) : 0.f;
    last_step = now;
    if(!positioned) {
        std::copy_n(sample.position,3,position.begin()); positioned=true;
    }
    // Preserve the requested position across ordinary ground correction. Abandon an
    // unrelated large relocation instead of dragging the player back to our old target.
    float discrepancy{};
    for(int i=0;i<3;++i) {
        const float delta=sample.position[i]-position[i]; discrepancy+=delta*delta;
    }
    if(!std::isfinite(discrepancy) || discrepancy>25.f) { reset(); return false; }
    const float length = std::sqrt(input.x*input.x + input.y*input.y + input.z*input.z);
    if (!std::isfinite(length)) { reset(); return false; }
    const float distance = speed * (input.fast ? 3.f : 1.f) * dt / std::max(length, 1.f);
    for (int i=0; i<3; ++i) {
        const float axis = i==0 ? input.x : i==1 ? input.y : input.z;
        target[i] = position[i] + axis * distance;
        if (!std::isfinite(target[i])) { reset(); return false; }
    }
    position=target;
    return true;
}

bool Override::prepare(const void* original, const Sample& sample, const std::array<float,3>& target) noexcept {
    __try {
        std::memcpy(view.data(), original, sizeof(view));
        if (view[18] != sample.row || sample.row >= 16384) return false;
        std::memcpy(transform.data(), reinterpret_cast<void*>(view[11] + sample.row*32ull), 32);
        std::memcpy(pushability.data(), reinterpret_cast<void*>(view[8] + sample.row*8ull), pushability.size());
        // The core still runs its contact/push solver after the keyframed branch
        // (0x1b99311 -> 0x2d79fb0). Disable that pass for this player call only.
        pushability[0]=0;
        for (int i=0; i<3; ++i) transform[4+i] = target[i];
        // These are integer ABI anchors, not C++ array pointers to dereference here.
        // The verified native routine adds row*stride before reading each component.
        view[11] = reinterpret_cast<uintptr_t>(transform.data()) - sample.row*32ull;
        view[7] = reinterpret_cast<uintptr_t>(keyframed.data()) - sample.row*2ull;
        view[8] = reinterpret_cast<uintptr_t>(pushability.data()) - sample.row*8ull;
        return true;
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
}
