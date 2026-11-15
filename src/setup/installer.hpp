#pragma once

// Installing into a game folder, and remembering which folders were done.
//
// Nothing here invents a new layout: an install is exactly the three files that
// were measured to work — the layer DLL, its implicit-layer manifest beside the
// game executable, and the ini the layer reads. The store is a plain ini under
// %APPDATA%, written through the Win32 profile API, so there is no parser to get
// wrong and no schema to migrate.

#include <filesystem>
#include <string>
#include <vector>

namespace reprovr::setup {

struct Game {
    int id{};
    std::wstring name;
    std::filesystem::path directory;
};

inline constexpr wchar_t kLayerDll[] = L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
inline constexpr wchar_t kLayerManifest[] = L"XR_APILAYER_XRFrameBridge_diagnostic.json";

std::filesystem::path store_path();
std::vector<Game> load_games();
// Returns the new id, or -1.
int add_game(const std::filesystem::path& directory, const std::wstring& name);
void remove_game(int id);
void rename_game(int id, const std::wstring& name);

// Auto-discovers VR and OpenXR games from Steam libraries.
std::vector<Game> discover_steam_games();
void auto_populate_games_if_empty();

bool installed(const std::filesystem::path& directory);
// `source` holds the DLL and the manifest of this very build.
bool install(const std::filesystem::path& game_directory,
             const std::filesystem::path& source,
             std::wstring* error);
bool uninstall(const std::filesystem::path& game_directory,
               std::wstring* error);

// True when the game process itself loads the installed proxy DLL: its .exe imports it,
// or a DLL of the folder it imports does. False = ReproVR may not start in that game.
bool proxy_will_load(const std::filesystem::path& game_directory);

// OpenXR Implicit Layer registry management
bool register_implicit_layer(const std::filesystem::path& manifest_path,
                             std::wstring* error = nullptr);
bool unregister_implicit_layer(const std::filesystem::path& manifest_path,
                               std::wstring* error = nullptr);
bool is_implicit_layer_registered(const std::filesystem::path& manifest_path);

// --- Legacy migration -------------------------------------------------------
// Older builds registered the layer in the GLOBAL OpenXR registry (ApiLayers\Implicit,
// HKCU and HKLM), loading ReproVR into every OpenXR app. ReproVR is per-game now.
struct LegacyGlobalLayer {
    bool machine_wide{};                 // HKLM (true) or HKCU (false)
    bool view32{};                       // 32-bit registry view (WOW6432Node)
    std::filesystem::path manifest;      // value name = manifest path
};
struct LegacyOptions {
    std::wstring implicit_subkey;        // empty = the real OpenXR Implicit key (tests override)
    bool include_machine = true;         // also scan HKLM
    bool dry_run = false;                // report only, change nothing
    std::filesystem::path backup_file;   // empty = %APPDATA%\ReproVR\legacy_global_backup.ini
    std::filesystem::path log_file;      // empty = %APPDATA%\ReproVR\migration.log
};
struct LegacyRepairReport {
    int found{};
    int removed{};
    int reinstalled{};                                // games re-armed per game
    std::vector<std::filesystem::path> needs_admin;   // HKLM entries we could not delete
    std::vector<std::filesystem::path> kept;          // left in place on purpose (re-arm/backup failed)
    std::vector<std::filesystem::path> not_rearmed;   // removed, game unknown: add it in the list
    std::vector<std::filesystem::path> unverified;    // re-armed, but the game may not load the proxy
    std::vector<std::wstring> log;
};
std::vector<LegacyGlobalLayer> find_legacy_global_layers(const LegacyOptions& options = {});
// Re-arms per game first, backs up, then removes. Never removes what it could not replace.
LegacyRepairReport repair_legacy_global_layers(const std::filesystem::path& source,
                                               const LegacyOptions& options = {});
// Puts back every entry saved in the backup file. Returns how many were restored.
int restore_legacy_global_layers(const LegacyOptions& options = {});
bool is_process_elevated();

// The folder this executable lives in: where the layer binaries are shipped.
std::filesystem::path source_directory();

}  // namespace reprovr::setup
