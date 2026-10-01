#include "crml.h"

static float elapsed, anchor[3];
static uint64_t generation, camera_generation;
static int32_t player_ready = -1, camera_ready = -1, camera_mode = -1;
#define MESSAGE(text) crml_log(1, text, sizeof(text) - 1)
uint32_t crml_abi_version(void) { return 1; }
void crml_init(void) {}
void crml_tick(float dt) {
    elapsed += dt;
    if (elapsed < 0.25f) return;
    elapsed = 0;
    crml_player_state player;
    const int ready = crml_player_read(&player, sizeof(player)) == 1;
    if (ready != player_ready) {
        if (ready) MESSAGE("State watch: player position available.");
        else MESSAGE("State watch: player position unavailable.");
        generation = 0;
    }
    player_ready = ready;
    if (ready) {
        float distance_squared = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const float delta = player.position[i] - anchor[i];
            distance_squared += delta * delta;
        }
        if (generation != player.generation || distance_squared >= 25.0f) {
            if (generation == player.generation) MESSAGE("State watch: moved at least five world units.");
            generation = player.generation;
            for (unsigned i = 0; i < 3; ++i) anchor[i] = player.position[i];
        }
    }
    crml_camera_state camera;
    const int camera_ok = crml_camera_read(&camera, sizeof(camera)) == 1;
    if (camera_ok) {
        if (camera_ready != 1 || camera_generation != camera.generation || camera_mode != camera.mode)
            MESSAGE("State watch: selected camera changed.");
        camera_generation = camera.generation;
        camera_mode = camera.mode;
    } else if (camera_ready != 0) {
        MESSAGE("State watch: camera unavailable.");
    }
    camera_ready = camera_ok;
}
