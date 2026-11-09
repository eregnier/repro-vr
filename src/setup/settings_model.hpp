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
         L"Turns everything off without uninstalling: the game runs as it normally "
         L"would, with no synthesised frames. The quickest way to compare with your "
         L"own eyes before changing anything else.",
         {L"1", L"0"}, {L"On", L"Off"}, L"General"},
        {L"ofxr", L"motion_vectors", Kind::choice,
         L"Motion vector source",
         L"Estimated lets the tool measure motion itself: the safest option, and the "
         L"one used as the reference. Game (DLSS) reuses the vectors the game hands to "
         L"its upscaler: more precise when they describe temporal motion, but some "
         L"games put something else there and the result degrades.",
         {L"off", L"dlss"}, {L"Estimated", L"Game (DLSS)"}, L"Reprojection"},
        {L"ofxr", L"backend", Kind::choice,
         L"Optical flow engine",
         L"FidelityFX works on any GPU. NVIDIA OFA uses the dedicated hardware in "
         L"recent cards: faster and often steadier, but unavailable elsewhere.",
         {L"fidelityfx", L"nvidia"}, {L"FidelityFX", L"NVIDIA OFA"}, L"Reprojection"},
        {L"ofxr", L"stereo_input_mode", Kind::choice,
         L"Stereo input mode",
         L"Auto detects what the game does. Native is for games that already render "
         L"both eyes in one frame (recommended for native VR titles). AER is for games "
         L"that render a single eye per frame.",
         {L"auto", L"native", L"aer"}, {L"Auto", L"Native stereo", L"AER"},
         L"Reprojection"},
        {L"ofxr", L"triple_frame_gen", Kind::choice,
         L"Frames per rendered pair",
         L"x2 inserts one frame between two game frames: the usual case. x3 inserts "
         L"two, for a higher target, at the price of one frame in three being fully "
         L"synthesised and therefore less faithful.",
         {L"0", L"1"}, {L"x2 (one added frame)", L"x3 (two added frames)"},
         L"Performance"},
        {L"ofxr", L"target_app_fps", Kind::choice,
         L"Application frame rate (Base)",
         L"The base frame rate rendered by the game engine (NOT the final headset refresh rate). "
         L"Auto locks the engine to HMD_Hz / Multiplier (e.g. 45 fps base for a 90 Hz headset in 2X). "
         L"Fixed values manually force this base cadence if auto oscillates.",
         {L"auto", L"45", L"60", L"72", L"90"},
         {L"Auto", L"45 fps", L"60 fps", L"72 fps", L"90 fps"}, L"Performance"},
        {L"ofxr", L"deep_pipeline", Kind::toggle,
         L"Deep pipeline",
         L"Full processing of frame pairs, which gives the better synthesis quality. "
         L"Turn it off only when GPU load becomes a problem.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Performance"},
        {L"ofxr", L"nvidia_preset", Kind::choice,
         L"NVIDIA OFA preset",
         L"Speed against quality for the hardware optical flow. No effect when the "
         L"engine is FidelityFX.",
         {L"slow", L"medium", L"fast"}, {L"Quality", L"Balanced", L"Speed"},
         L"Performance"},
        {L"ofxr", L"nvidia_input_scale", Kind::choice,
         L"OFA input resolution",
         L"Flow is estimated on a reduced image. 50% costs far less but loses small "
         L"motions; 100% is the reference.",
         {L"100", L"75", L"50"}, {L"100%", L"75%", L"50%"}, L"Performance"},
        {L"ofxr", L"nvidia_bidirectional", Kind::toggle,
         L"Bidirectional flow",
         L"Also estimates the backward flow, which doubles the estimation cost and "
         L"improves how coherent the synthesised frames are. No effect with FidelityFX.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Performance"},
        {L"overlay", L"position", Kind::choice,
         L"On-screen counters",
         L"Shows the real frame rate and the number of synthesised frames, in a corner "
         L"of the headset. Handy while measuring, off the rest of the time.",
         {L"off", L"upper_left", L"upper_right", L"lower_left", L"lower_right"},
         {L"Off", L"Top left", L"Top right", L"Bottom left", L"Bottom right"},
         L"Diagnostics"},
        {L"diagnostics", L"logging_enabled", Kind::toggle,
         L"Detailed log",
         L"Writes a session log next to the game. The cost is negligible, but the file "
         L"grows quickly: enable it only to understand a problem.",
         {L"1", L"0"}, {L"On", L"Off"}, L"Diagnostics"},
        {L"diagnostics", L"dump_guides", Kind::toggle,
         L"Capture debug guides",
         L"Writes diagnostic textures on request, for offline analysis. Of no use in "
         L"normal play, and expensive while it is active.",
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
