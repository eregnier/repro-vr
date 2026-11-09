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

// The folder this executable lives in: where the layer binaries are shipped.
std::filesystem::path source_directory();

}  // namespace reprovr::setup
