#pragma once

// The settings this tool exposes, and nothing else.
//
// Every real setting of the layer is either a toggle or a closed enumeration, so
// the interface offers no free text at all: an out-of-range value cannot be typed
// because it cannot be typed. The table below is the single source of truth for
// the labels, the tooltips and what the reader in the layer accepts.

#include <windows.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace reprovr::setup {

enum class Kind {
    // 0/1 written as "0"/"1".
    toggle,
    // One of `values`, written verbatim.
    choice,
};

struct Setting {
    const wchar_t* section;
    const wchar_t* key;
    Kind kind;
    const wchar_t* title;
    const wchar_t* help;
    // Raw values written to the ini; labels shown to the user; the first index is
    // the default when the key is absent.
    std::vector<const wchar_t*> values;
    std::vector<const wchar_t*> labels;
    // Card this row belongs to.
    const wchar_t* group;
};

inline constexpr int kDefaultIndex = 0;

inline const std::vector<Setting>& settings() {
    static const std::vector<Setting> table{
        {L"ofxr", L"enabled", Kind::toggle,
         L"Enable reprojection",
         L"Purpose: Master toggle to enable or bypass frame synthesis.\n"
         L"Impact: When Off, the game runs at its native unassisted frame rate with zero synthesis overhead. Ideal for immediate A/B visual comparison.",
         {L"1", L"0"}, {L"On", L"Off"}, L"General"},
        {L"ofxr", L"motion_vectors", Kind::choice,
         L"Motion vector source",
         L"Purpose: Selects how motion vectors are calculated.\n"
         L"Impact: 'Estimated' uses GPU optical flow and works universally on all games. 'Game (DLSS)' reuses engine motion vectors for sharper geometry edges, but may artifact in games with non-standard vector encodings.",
         {L"off", L"dlss"}, {L"Estimated", L"Game (DLSS)"}, L"Reprojection"},
        {L"ofxr", L"backend", Kind::choice,
         L"Optical flow engine",
         L"Purpose: Chooses the optical flow computation engine.\n"
         L"Impact: 'NVIDIA OFA' runs on dedicated RTX hardware chips (~1 ms GPU time, zero compute core load). 'FidelityFX' runs on general compute shaders on any GPU (AMD/Intel/NVIDIA), but uses compute ALU headroom.",
         {L"fidelityfx", L"nvidia"}, {L"FidelityFX", L"NVIDIA OFA"}, L"Reprojection"},
        {L"ofxr", L"stereo_input_mode", Kind::choice,
         L"Stereo input mode",
         L"Purpose: Configures the stereo rendering pipeline.\n"
         L"Impact: 'Native' is recommended for VR games rendering both eyes per frame. 'AER' (Alternate Eye Rendering) is for flat-to-VR mods (Luke Ross, UEVR AER) that render one eye per frame, cutting geometry load in half.",
         {L"auto", L"native", L"aer"}, {L"Auto", L"Native stereo", L"AER"},
         L"Reprojection"},
        {L"ofxr", L"triple_frame_gen", Kind::choice,
         L"Frames per rendered pair",
         L"Purpose: Chooses between 2X and 3X frame synthesis.\n"
         L"Impact: 'x2' inserts 1 synthetic frame per real frame (e.g. 45 -> 90 Hz). 'x3' inserts 2 synthetic frames (e.g. 40 -> 120 Hz) for high-refresh headsets, lowering game CPU/GPU demand even further.",
         {L"0", L"1"}, {L"x2 (one added frame)", L"x3 (two added frames)"},
         L"Performance"},
        {L"ofxr", L"target_app_fps", Kind::choice,
         L"Application frame rate (Base)",
         L"Purpose: Locks the base game engine rendering pace (NOT the headset display refresh rate).\n"
         L"Impact: 'Auto' locks the engine to HMD_Hz / Multiplier (e.g. 45 fps base for a 90 Hz headset in 2X). Eliminates cadence judder. Fixed values manually force this base cadence if auto oscillates.",
         {L"auto", L"45", L"60", L"72", L"90"},
         {L"Auto", L"45 fps", L"60 fps", L"72 fps", L"90 fps"}, L"Performance"},
        {L"ofxr", L"deep_pipeline", Kind::toggle,
         L"Deep pipeline",
         L"Purpose: Allocates a deeper frame queue buffer for synthesis.\n"
         L"Impact: Maximizes synthesis stability and smooths out momentary game hitches. Adds ~1 frame of latency; disable if latency or VRAM is tight.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Performance"},
        {L"ofxr", L"nvidia_preset", Kind::choice,
         L"NVIDIA OFA preset",
         L"Purpose: Adjusts the quality vs performance preset of NVIDIA Optical Flow.\n"
         L"Impact: 'Quality' produces the finest vector detail. 'Speed' minimizes optical flow execution time. No effect on FidelityFX.",
         {L"slow", L"medium", L"fast"}, {L"Quality", L"Balanced", L"Speed"},
         L"Performance"},
        {L"ofxr", L"nvidia_input_scale", Kind::choice,
         L"OFA input resolution",
         L"Purpose: Downscales the resolution used for motion estimation.\n"
         L"Impact: '50%' cuts optical flow compute time by ~3x with great results for fast action. '100%' offers maximum accuracy for tiny distant objects.",
         {L"100", L"75", L"50"}, {L"100%", L"75%", L"50%"}, L"Performance"},
        {L"ofxr", L"nvidia_bidirectional", Kind::toggle,
         L"Bidirectional flow",
         L"Purpose: Estimates optical flow in both forward and backward directions.\n"
         L"Impact: Improves temporal consistency and reduces occlusion boundary warping, but doubles optical flow calculation time. No effect on FidelityFX.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Performance"},
        {L"overlay", L"position", Kind::choice,
         L"On-screen counters",
         L"Purpose: Displays a floating real-time performance HUD inside the headset.\n"
         L"Impact: Shows base rendered FPS vs synthesized HMD refresh rate in your chosen corner. Negligible performance overhead.",
         {L"off", L"upper_left", L"upper_right", L"lower_left", L"lower_right"},
         {L"Off", L"Top left", L"Top right", L"Bottom left", L"Bottom right"},
         L"Diagnostics"},
        {L"diagnostics", L"logging_enabled", Kind::toggle,
         L"Detailed log",
         L"Purpose: Records verbose frame timing and state transitions to repro_vr.log.\n"
         L"Impact: Negligible GPU impact, but log file grows quickly. Keep Off unless investigating an issue.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Diagnostics"},
        {L"diagnostics", L"dump_guides", Kind::toggle,
         L"Capture debug guides",
         L"Purpose: Dumps raw optical flow vectors and depth textures to disk for offline analysis.\n"
         L"Impact: Causes severe frame drops while writing files. For mod development only.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Diagnostics"},
    };
    return table;
}

inline std::wstring ini_path(const std::filesystem::path& game_directory) {
    return (game_directory / L"repro_vr.ini").wstring();
}

// Returns the index of the value currently written, or the default index. An
// unrecognised value falls back to the default rather than being echoed back:
// the interface must never display a value the layer cannot honour.
inline int read_setting(const Setting& setting,
                        const std::filesystem::path& game_directory) {
    if (game_directory.empty()) return kDefaultIndex;
    const std::wstring path = ini_path(game_directory);
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(setting.section, setting.key, L"",
                             buffer, static_cast<DWORD>(std::size(buffer)),
                             path.c_str());
    for (std::size_t i = 0; i < setting.values.size(); ++i) {
        if (_wcsicmp(buffer, setting.values[i]) == 0) return static_cast<int>(i);
    }
    return kDefaultIndex;
}

inline bool write_setting(const Setting& setting, int index,
                          const std::filesystem::path& game_directory) {
    if (game_directory.empty()) return false;
    if (index < 0 || index >= static_cast<int>(setting.values.size())) return false;
    const wchar_t* value = setting.values[static_cast<std::size_t>(index)];
    // Both names, always. The layer resolves `repro_vr.ini` first, but its
    // embedded control block reads `ofxr_bridge.ini` directly, so a key written
    // to one file only would be honoured by half the readers. The two are written
    // together so they can never disagree.
    bool ok = true;
    for (const wchar_t* name : {L"repro_vr.ini", L"ofxr_bridge.ini"}) {
        const std::wstring path = (game_directory / name).wstring();
        ok = WritePrivateProfileStringW(setting.section, setting.key, value,
                                        path.c_str()) != FALSE && ok;
    }
    return ok;
}

}  // namespace reprovr::setup
