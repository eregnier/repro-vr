#include "installer.hpp"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace reprovr::setup {
namespace {

constexpr wchar_t kStoreFolder[] = L"ReproVR";
constexpr wchar_t kStoreFile[] = L"games.ini";
constexpr wchar_t kMetaSection[] = L"meta";
constexpr wchar_t kNextKey[] = L"next";

std::wstring game_section(int id) {
    return L"game:" + std::to_wstring(id);
}

std::wstring read_string(const wchar_t* section, const wchar_t* key,
                         const std::filesystem::path& file) {
    wchar_t buffer[1024]{};
    const DWORD length = GetPrivateProfileStringW(
        section, key, L"", buffer, static_cast<DWORD>(std::size(buffer)),
        file.c_str());
    return std::wstring(buffer, length);
}

void write_string(const wchar_t* section, const wchar_t* key,
                  const std::wstring& value,
                  const std::filesystem::path& file) {
    WritePrivateProfileStringW(section, key, value.c_str(), file.c_str());
}

std::filesystem::path appdata_directory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) ||
        raw == nullptr) {
        return {};
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

bool copy_if_needed(const std::filesystem::path& from,
                    const std::filesystem::path& to, std::wstring* error) {
    std::error_code code;
    if (!std::filesystem::exists(from, code) || code) {
        if (error) *error = L"Missing file: " + from.wstring();
        return false;
    }
    // A running game holds the DLL open: replace it only when the bytes differ,
    // so re-running the installer on an unchanged build never fails on a lock.
    if (std::filesystem::exists(to, code) && !code &&
        std::filesystem::file_size(from, code) == std::filesystem::file_size(to, code) &&
        !code) {
        std::ifstream a(from, std::ios::binary), b(to, std::ios::binary);
        if (a && b && std::equal(std::istreambuf_iterator<char>(a),
                                 std::istreambuf_iterator<char>(),
                                 std::istreambuf_iterator<char>(b))) {
            return true;
        }
    }
    std::filesystem::copy_file(from, to,
                               std::filesystem::copy_options::overwrite_existing,
                               code);
    if (code) {
        if (error) *error = L"Copy failed: " + to.wstring();
        return false;
    }
    return true;
}

// The manifest beside the game executable is the only discovery route measured to
// work for this layer, so the installer writes exactly that and nothing else.
bool write_manifest(const std::filesystem::path& game_directory,
                    const std::filesystem::path& dll_path, std::wstring* error) {
    std::wstring escaped;
    for (const wchar_t ch : dll_path.wstring()) {
        if (ch == L'\\') escaped += L"\\\\";
        else escaped += ch;
    }
    const std::wstring json =
        L"{\n"
        L"  \"file_format_version\": \"1.0.0\",\n"
        L"  \"api_layer\": {\n"
        L"    \"name\": \"XR_APILAYER_XRFrameBridge_diagnostic\",\n"
        L"    \"library_path\": \"" + escaped + L"\",\n"
        L"    \"api_version\": \"1.0\",\n"
        L"    \"implementation_version\": \"1\",\n"
        L"    \"description\": \"ReproVR 6-DOF Depth & Optical Flow Frame Synthesis Layer\",\n"
        L"    \"disable_environment\": \"XRFG_DISABLE_OFXR_BRIDGE\"\n"
        L"  }\n"
        L"}\n";
    std::ofstream out(game_directory / kLayerManifest,
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = L"Cannot write the manifest into " + game_directory.wstring();
        return false;
    }
    // The manifest is ASCII by construction; write narrow bytes as they are.
    std::string narrow;
    narrow.reserve(json.size());
    for (const wchar_t ch : json) narrow += static_cast<char>(ch);
    out.write(narrow.data(), static_cast<std::streamsize>(narrow.size()));
    return out.good();
}

}  // namespace

std::filesystem::path store_path() {
    const auto base = appdata_directory();
    if (base.empty()) return {};
    std::error_code code;
    std::filesystem::create_directories(base / kStoreFolder, code);
    return base / kStoreFolder / kStoreFile;
}

std::vector<Game> load_games() {
    std::vector<Game> games;
    const auto file = store_path();
    if (file.empty()) return games;
    const int next = GetPrivateProfileIntW(kMetaSection, kNextKey, 1, file.c_str());
    for (int id = 1; id < next; ++id) {
        const std::wstring section = game_section(id);
        const std::wstring path = read_string(section.c_str(), L"path", file);
        if (path.empty()) continue;
        std::wstring name = read_string(section.c_str(), L"name", file);
        if (name.empty()) {
            name = std::filesystem::path(path).parent_path().filename().wstring();
        }
        games.push_back({id, name, std::filesystem::path(path)});
    }
    return games;
}

int add_game(const std::filesystem::path& directory, const std::wstring& name) {
    const auto file = store_path();
    if (file.empty() || directory.empty()) return -1;
    for (const auto& game : load_games()) {
        if (game.directory == directory) return game.id;
    }
    int next = GetPrivateProfileIntW(kMetaSection, kNextKey, 1, file.c_str());
    if (next < 1) next = 1;
    const int id = next;
    const std::wstring section = game_section(id);
    write_string(section.c_str(), L"path", directory.wstring(), file);
    std::wstring label = name.empty()
        ? directory.parent_path().filename().wstring() : name;
    if (label.empty()) label = directory.filename().wstring();
    write_string(section.c_str(), L"name", label, file);
    write_string(kMetaSection, kNextKey, std::to_wstring(id + 1), file);
    return id;
}

void remove_game(int id) {
    const auto file = store_path();
    if (file.empty()) return;
    WritePrivateProfileStringW(game_section(id).c_str(), nullptr, nullptr,
                               file.c_str());
}

void rename_game(int id, const std::wstring& name) {
    const auto file = store_path();
    if (file.empty() || name.empty()) return;
    write_string(game_section(id).c_str(), L"name", name, file);
}

namespace {

std::vector<std::filesystem::path> detect_steam_libraries() {
    std::vector<std::filesystem::path> libraries;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t steamPath[MAX_PATH] = {};
        DWORD size = sizeof(steamPath);
        if (RegQueryValueExW(hKey, L"SteamPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(steamPath), &size) == ERROR_SUCCESS) {
            std::filesystem::path base(steamPath);
            std::error_code ec;
            if (std::filesystem::exists(base, ec)) {
                libraries.push_back(base);
            }
            std::filesystem::path vdf = base / L"steamapps" / L"libraryfolders.vdf";
            std::ifstream file(vdf);
            if (file.is_open()) {
                std::string line;
                while (std::getline(file, line)) {
                    auto pos = line.find("\"path\"");
                    if (pos != std::string::npos) {
                        auto first = line.find('"', pos + 6);
                        if (first != std::string::npos) {
                            auto second = line.find('"', first + 1);
                            if (second != std::string::npos) {
                                std::string p = line.substr(first + 1, second - first - 1);
                                std::string clean;
                                for (size_t i = 0; i < p.size(); ++i) {
                                    if (p[i] == '\\' && i + 1 < p.size() && p[i+1] == '\\') {
                                        clean += '\\';
                                        ++i;
                                    } else {
                                        clean += p[i];
                                    }
                                }
                                std::filesystem::path libPath(clean);
                                if (std::filesystem::exists(libPath, ec)) {
                                    bool found = false;
                                    for (const auto& existing : libraries) {
                                        if (std::filesystem::equivalent(existing, libPath, ec)) {
                                            found = true;
                                            break;
                                        }
                                    }
                                    if (!found) libraries.push_back(libPath);
                                }
                            }
                        }
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
    return libraries;
}

}  // namespace

std::vector<Game> discover_steam_games() {
    std::vector<Game> results;
    std::error_code ec;
    const auto libs = detect_steam_libraries();

    for (const auto& lib : libs) {
        const auto common = lib / L"steamapps" / L"common";
        if (!std::filesystem::exists(common, ec)) continue;

        for (const auto& entry : std::filesystem::directory_iterator(
                 common, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_directory(ec)) continue;

            const std::wstring folderName = entry.path().filename().wstring();
            std::filesystem::path targetExeDir;
            std::wstring friendlyName = folderName;

            // 1. Half-Life: Alyx
            if (_wcsicmp(folderName.c_str(), L"Half-Life Alyx") == 0) {
                auto alyxDir = entry.path() / L"game" / L"bin" / L"win64";
                if (std::filesystem::exists(alyxDir / L"hlvr.exe", ec)) {
                    targetExeDir = alyxDir;
                    friendlyName = L"Half-Life: Alyx";
                }
            }
            // 2. Subnautica
            else if (_wcsicmp(folderName.c_str(), L"Subnautica") == 0) {
                if (std::filesystem::exists(entry.path() / L"Subnautica.exe", ec)) {
                    targetExeDir = entry.path();
                    friendlyName = L"Subnautica";
                }
            }
            // 3. Subnautica: Below Zero
            else if (_wcsicmp(folderName.c_str(), L"SubnauticaZero") == 0 ||
                     _wcsicmp(folderName.c_str(), L"Subnautica Below Zero") == 0) {
                if (std::filesystem::exists(entry.path() / L"SubnauticaZero.exe", ec)) {
                    targetExeDir = entry.path();
                    friendlyName = L"Subnautica: Below Zero";
                }
            }
            // 4. Skyrim VR
            else if (_wcsicmp(folderName.c_str(), L"SkyrimVR") == 0) {
                if (std::filesystem::exists(entry.path() / L"SkyrimVR.exe", ec)) {
                    targetExeDir = entry.path();
                    friendlyName = L"The Elder Scrolls V: Skyrim VR";
                }
            }
            // 5. Half-Life 2 VR
            else if (_wcsicmp(folderName.c_str(), L"Half-Life 2 VR") == 0) {
                if (std::filesystem::exists(entry.path() / L"hl2.exe", ec)) {
                    targetExeDir = entry.path();
                    friendlyName = L"Half-Life 2: VR Mod";
                }
            }
            // 6. Generic VR detection: look for OpenXR/OpenVR binaries or UEVR patterns
            else {
                // Check root directory
                bool hasVrDll = std::filesystem::exists(entry.path() / L"openvr_api.dll", ec) ||
                                std::filesystem::exists(entry.path() / L"openxr_loader.dll", ec);
                if (hasVrDll) {
                    targetExeDir = entry.path();
                } else {
                    // Check Binaries/Win64 or bin/win64 or bin/x64
                    for (const auto& sub : {L"Binaries\\Win64", L"bin\\win64", L"bin\\x64"}) {
                        auto cand = entry.path() / sub;
                        if (std::filesystem::exists(cand, ec)) {
                            if (std::filesystem::exists(cand / L"openvr_api.dll", ec) ||
                                std::filesystem::exists(cand / L"openxr_loader.dll", ec) ||
                                std::filesystem::exists(cand / L"repro_vr.ini", ec)) {
                                targetExeDir = cand;
                                break;
                            }
                        }
                    }
                }
            }

            if (!targetExeDir.empty()) {
                results.push_back({0, friendlyName, targetExeDir});
            }
        }
    }
    return results;
}

void auto_populate_games_if_empty() {
    auto current = load_games();
    if (!current.empty()) return;

    auto discovered = discover_steam_games();
    for (const auto& g : discovered) {
        add_game(g.directory, g.name);
    }
}

std::filesystem::path source_directory() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path,
                                            static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    return std::filesystem::path(path).parent_path();
}

bool installed(const std::filesystem::path& directory) {
    std::error_code code;
    if (directory.empty()) return false;
    return std::filesystem::exists(directory / kLayerDll, code) && !code &&
           std::filesystem::exists(directory / kLayerManifest, code) && !code;
}

bool install(const std::filesystem::path& game_directory,
             const std::filesystem::path& source, std::wstring* error) {
    std::error_code code;
    if (game_directory.empty() || source.empty()) return false;
    if (!std::filesystem::is_directory(game_directory, code) || code) {
        if (error) *error = L"Folder not found: " + game_directory.wstring();
        return false;
    }
    const auto dll_target = game_directory / kLayerDll;
    if (!copy_if_needed(source / kLayerDll, dll_target, error)) return false;
    if (!write_manifest(game_directory, dll_target, error)) return false;
    // Both names: the layer resolves repro_vr.ini first, but its embedded
    // control block reads ofxr_bridge.ini directly, so a key written to one
    // file is honoured by half the readers and silently ignored by the rest.
    for (const wchar_t* name : {L"repro_vr.ini", L"ofxr_bridge.ini"}) {
        const auto ini = game_directory / name;
        if (!std::filesystem::exists(ini, code) || code) {
            std::ofstream(ini, std::ios::app)
                << "; ReproVR configuration\n[ofxr]\n";
        }
    }
    return true;
}

bool uninstall(const std::filesystem::path& game_directory, std::wstring* error) {
    std::error_code code;
    if (game_directory.empty()) return false;
    for (const wchar_t* name : {kLayerDll, kLayerManifest}) {
        std::filesystem::remove(game_directory / name, code);
        if (code) {
            if (error) {
                *error = L"Cannot delete: " +
                         (game_directory / name).wstring() +
                         L" (is the game still running?)";
            }
            return false;
        }
    }
    // repro_vr.ini holds the user's settings: uninstalling leaves it alone.
    return true;
}

}  // namespace reprovr::setup
