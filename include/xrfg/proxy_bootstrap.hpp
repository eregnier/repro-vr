#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace xrfg {

struct ProxyBootstrapResult {
    bool system_version_loaded{};
    bool layer_dll_found{};
    bool manifest_ready{};
    bool environment_configured{};
    std::filesystem::path layer_dll_path;
    std::filesystem::path manifest_path;
};

struct GameDropInInstallResult {
    bool succeeded{};
    std::filesystem::path game_directory;
    std::filesystem::path installed_proxy_dll;
    std::filesystem::path installed_layer_dll;
    std::filesystem::path installed_manifest_json;
    std::wstring error_message;
};

// Generates a valid Khronos OpenXR API layer JSON manifest pointing to layer_dll_path.
[[nodiscard]] bool write_openxr_layer_manifest_json(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& layer_dll_path) noexcept;

// Configures the current process environment (XR_API_LAYER_PATH and XR_ENABLE_API_LAYERS)
// to load ReproVR from base_directory without requiring Windows Registry keys.
[[nodiscard]] ProxyBootstrapResult bootstrap_repro_vr_from_directory(
    const std::filesystem::path& base_directory) noexcept;

// One-click frictionless installation of the ReproVR drop-in bundle into a game folder.
[[nodiscard]] GameDropInInstallResult install_dropin_to_game_directory(
    const std::filesystem::path& source_bundle_dir,
    const std::filesystem::path& target_game_dir) noexcept;

// Frictionless removal of ReproVR drop-in files from a game folder.
[[nodiscard]] bool remove_dropin_from_game_directory(
    const std::filesystem::path& target_game_dir) noexcept;

}  // namespace xrfg
