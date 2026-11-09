#pragma once
#include <cstdint>
#include <filesystem>

namespace xrfg::embedded {
// CPU-only interface: no NGX/FFX structures cross the host/backend ABI boundary.
struct Settings {
    bool enabled{true};
    int backend{}; // 0 FidelityFX, 1 NVIDIA
    int preset{1}; // 0 slow, 1 medium, 2 fast
    int scale{2}; // 0 full, 1 three-quarter, 2 half
    bool backward{};
    int motion_vectors{}; // 0 optical flow, 1 DLSS ingress
    int frame_generation{}; // ABI compatibility: standalone accepts only 0.
    bool operator==(const Settings&) const = default;
};
struct Snapshot {
    Settings desired{};
    std::uint64_t revision{1};
    unsigned sessions{}, pending{}, bypass{}, ready{}, errors{};
    long last_error{};
};
Snapshot snapshot();
// Reads the control keys from one INI, honouring the same section order the
// layer uses at start-up: [repro_vr], then [reprovr], then [ofxr]. Pure: no
// state, no writes, so a caller can compare without side effects.
Settings read_settings_file(const std::filesystem::path& ini_path);
// Re-reads the module's own INI and, if a key changed on disk, applies it the
// same way an in-process request would. This is what makes the settings editable
// while the game runs: true when the applied settings changed.
bool reload_from_ini();
// reload_from_ini() at most once every half second; cheap enough to call per
// frame, which is where the frame loop calls it.
void poll_ini();
bool request(Settings settings); // persists only owned keys; false means INI write failed
std::uint64_t attach();
void detach(std::uint64_t id);
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error);
bool logging_setting();
bool set_logging_setting(bool enabled); // the recorder follows its INI live
int overlay_setting();
bool set_overlay_setting(int position); // existing overlay polls its INI live
}
