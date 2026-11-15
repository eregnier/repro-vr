#include "installer.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
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

constexpr wchar_t kInstallMarkerName[] = L"repro_vr_installed.manifest";
constexpr const wchar_t* kProxyCandidates[] = {L"version.dll", L"winmm.dll", L"dxgi.dll"};

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

// --- Which proxy DLL does the game really load? ------------------------------
// The proxy only works if the game loads a DLL of that name from its own folder.
// Measured 2026-11-15 on real installs: SkyrimVR.exe imports dxgi/winmm but NOT
// version.dll, so the old "first free name" choice (version.dll) never loaded there.

// Lower-cased names of the DLLs a PE file imports (normal + delay-load). Read-only
// mapping, every offset bounds-checked: a damaged or foreign file yields {}.
std::vector<std::string> pe_imported_dlls(const std::filesystem::path& file) {
    std::vector<std::string> out;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    HANDLE m = size.QuadPart > 0 ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr)
                                 : nullptr;
    const auto* base = m ? static_cast<const unsigned char*>(
                               MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0))
                         : nullptr;
    const size_t n = static_cast<size_t>(size.QuadPart);
    auto ok = [&](size_t off, size_t len) { return off <= n && len <= n - off; };
    auto u16 = [&](size_t off) { return *reinterpret_cast<const WORD*>(base + off); };
    auto u32 = [&](size_t off) { return *reinterpret_cast<const DWORD*>(base + off); };

    if (base != nullptr && ok(0, 0x40) && u16(0) == IMAGE_DOS_SIGNATURE) {
        const size_t nt = u32(0x3C);
        if (ok(nt, 24) && u32(nt) == IMAGE_NT_SIGNATURE) {
            const size_t fh = nt + 4;
            const WORD sections = u16(fh + 2);
            const WORD opt_size = u16(fh + 16);
            const size_t opt = fh + 20;
            if (ok(opt, 2)) {
                const WORD magic = u16(opt);
                const bool pe64 = magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
                const size_t dirs = opt + (pe64 ? 112 : 96);
                const unsigned long long image_base =
                    pe64 ? (ok(opt + 24, 8) ? *reinterpret_cast<const ULONGLONG*>(base + opt + 24) : 0)
                         : (ok(opt + 28, 4) ? u32(opt + 28) : 0);
                const size_t sect = opt + opt_size;
                auto rva_to_off = [&](DWORD rva, size_t* off) {
                    for (WORD i = 0; i < sections; ++i) {
                        const size_t s = sect + static_cast<size_t>(i) * 40;
                        if (!ok(s, 40)) return false;
                        const DWORD va = u32(s + 12), vsize = u32(s + 8);
                        const DWORD raw = u32(s + 20), rsize = u32(s + 16);
                        const DWORD span = vsize > rsize ? vsize : rsize;
                        if (rva >= va && rva < va + span) {
                            *off = static_cast<size_t>(raw) + (rva - va);
                            return true;
                        }
                    }
                    return false;
                };
                auto read_name = [&](DWORD rva) {
                    size_t off = 0;
                    std::string s;
                    if (!rva_to_off(rva, &off)) return s;
                    while (off < n && base[off] != 0 && s.size() < 260) {
                        s += static_cast<char>(tolower(base[off++]));
                    }
                    return s;
                };
                // Import directory (index 1): 20-byte descriptors, Name at +12.
                if (ok(dirs + 8 * 1, 8)) {
                    size_t off = 0;
                    if (u32(dirs + 8) != 0 && rva_to_off(u32(dirs + 8), &off)) {
                        for (int i = 0; i < 4096 && ok(off, 20); ++i, off += 20) {
                            const DWORD name = u32(off + 12);
                            if (name == 0) break;
                            out.push_back(read_name(name));
                        }
                    }
                }
                // Delay-load directory (index 13): 32-byte descriptors, name at +4.
                if (ok(dirs + 8 * 13, 8)) {
                    size_t off = 0;
                    if (u32(dirs + 8 * 13) != 0 && rva_to_off(u32(dirs + 8 * 13), &off)) {
                        for (int i = 0; i < 4096 && ok(off, 32); ++i, off += 32) {
                            const DWORD attrs = u32(off);
                            DWORD name = u32(off + 4);
                            if (name == 0) break;
                            if ((attrs & 1) == 0 && image_base != 0 && name > image_base) {
                                name = static_cast<DWORD>(name - image_base);  // old VA-based form
                            }
                            out.push_back(read_name(name));
                        }
                    }
                }
            }
        }
    }
    if (base != nullptr) UnmapViewOfFile(base);
    if (m != nullptr) CloseHandle(m);
    CloseHandle(f);
    return out;
}

// Helper executables that ship beside a game but are not the game process. Measured
// 2026-11-15: in PLAYA VR only UnityCrashHandler64.exe imports version.dll, the game
// itself (PlayaVR.exe -> UnityPlayer.dll) imports dxgi.dll.
bool is_helper_executable(const std::wstring& lowered_name) {
    for (const wchar_t* word : {L"crash", L"unins", L"redist", L"prereq", L"setup",
                                L"install", L"report", L"eac", L"easyanticheat",
                                L"be_service", L"launcherhelper"}) {
        if (lowered_name.find(word) != std::wstring::npos) return true;
    }
    return false;
}

// How strongly the game folder says the game process loads `dll` (lower-case):
//   3 = a game .exe of the folder imports it directly,
//   2 = reached through the game's own import chain (exe -> DLL of the folder -> ...),
//   1 = only some other DLL of the folder imports it (may never be loaded),
//   0 = nothing does.
int proxy_load_evidence(const std::filesystem::path& game_directory, const wchar_t* dll) {
    std::string wanted;
    for (const wchar_t* p = dll; *p; ++p) wanted += static_cast<char>(towlower(*p));

    // Index the folder once: lower-case file name -> path.
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             game_directory, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (files.size() >= 800) break;
        std::wstring ext = entry.path().extension().wstring();
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
        if (ext != L".exe" && ext != L".dll") continue;
        std::string name;
        for (const wchar_t ch : entry.path().filename().wstring()) {
            name += static_cast<char>(towlower(ch));
        }
        if (name == wanted) continue;  // the proxy itself
        files.emplace_back(name, entry.path());
    }
    auto find_file = [&](const std::string& name) -> const std::filesystem::path* {
        for (const auto& f : files) {
            if (f.first == name) return &f.second;
        }
        return nullptr;
    };

    int best = 0;
    // Walk the import graph from every game executable (depth-limited, no revisits).
    std::vector<std::string> visited;
    for (const auto& f : files) {
        const std::wstring wname = f.second.filename().wstring();
        std::wstring lowered;
        for (const wchar_t ch : wname) lowered += static_cast<wchar_t>(towlower(ch));
        if (f.first.size() < 4 || f.first.substr(f.first.size() - 4) != ".exe" ||
            is_helper_executable(lowered)) {
            continue;
        }
        std::vector<std::pair<std::filesystem::path, int>> queue{{f.second, 0}};
        while (!queue.empty() && best < 3) {
            const auto [file, depth] = queue.back();
            queue.pop_back();
            for (const auto& imp : pe_imported_dlls(file)) {
                if (imp == wanted) {
                    best = std::max(best, depth == 0 ? 3 : 2);
                    continue;
                }
                if (depth >= 3) continue;
                const auto* next = find_file(imp);
                if (next == nullptr ||
                    std::find(visited.begin(), visited.end(), imp) != visited.end()) {
                    continue;
                }
                visited.push_back(imp);
                queue.emplace_back(*next, depth + 1);
            }
        }
        if (best == 3) return best;
    }
    if (best > 0) return best;

    // Weak evidence: some DLL of the folder imports it.
    for (const auto& f : files) {
        if (f.first.size() < 4 || f.first.substr(f.first.size() - 4) != ".dll") continue;
        for (const auto& imp : pe_imported_dlls(f.second)) {
            if (imp == wanted) return 1;
        }
    }
    return 0;
}

bool is_our_proxy(const std::filesystem::path& file) {
    HMODULE probe = LoadLibraryExW(file.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (probe == nullptr) return false;
    const bool ours = GetProcAddress(probe, "ReproVR_GetProxyBootstrapStatus") != nullptr;
    FreeLibrary(probe);
    return ours;
}

std::wstring read_marker_proxy(const std::filesystem::path& marker_path) {
    std::ifstream mf(marker_path);
    std::string line;
    while (std::getline(mf, line)) {
        if (line.rfind("proxy=", 0) == 0 && line.size() > 6) {
            return std::wstring(line.begin() + 6, line.end());
        }
    }
    return {};
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

bool register_implicit_layer(const std::filesystem::path& manifest_path,
                             std::wstring* error) {
    std::error_code ec;
    const std::wstring abs_path =
        std::filesystem::absolute(manifest_path, ec).lexically_normal().wstring();
    if (abs_path.empty()) {
        if (error) *error = L"Invalid manifest path";
        return false;
    }

    constexpr DWORD enabled_value = 0;
    bool ok_hkcu = false;

    // 1. Register in HKCU Implicit (loaded automatically by OpenXR runtimes)
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
        0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
        nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS && key != nullptr) {
        status = RegSetValueExW(
            key,
            abs_path.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled_value),
            sizeof(enabled_value));
        ok_hkcu = (status == ERROR_SUCCESS);
        RegCloseKey(key);
    }

    // 2. Also register in HKCU Explicit for full compatibility
    key = nullptr;
    status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit",
        0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
        nullptr, &key, nullptr);
    if (status == ERROR_SUCCESS && key != nullptr) {
        RegSetValueExW(
            key,
            abs_path.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled_value),
            sizeof(enabled_value));
        RegCloseKey(key);
    }

    // 3. Register in HKLM Implicit & Explicit (best-effort if elevated)
    for (const wchar_t* subkey : {
             L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
             L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit"}) {
        key = nullptr;
        status = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            subkey,
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            nullptr, &key, nullptr);
        if (status == ERROR_SUCCESS && key != nullptr) {
            RegSetValueExW(
                key,
                abs_path.c_str(),
                0,
                REG_DWORD,
                reinterpret_cast<const BYTE*>(&enabled_value),
                sizeof(enabled_value));
            RegCloseKey(key);
        }
    }

    if (!ok_hkcu && error) {
        *error = L"Failed to register OpenXR implicit layer in HKCU registry";
    }
    return ok_hkcu;
}

bool unregister_implicit_layer(const std::filesystem::path& manifest_path,
                               std::wstring* error) {
    (void)error;
    std::error_code ec;
    const std::wstring abs_path =
        std::filesystem::absolute(manifest_path, ec).lexically_normal().wstring();
    if (abs_path.empty()) return true;

    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const wchar_t* subkey : {
                 L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
                 L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit"}) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS && key != nullptr) {
                RegDeleteValueW(key, abs_path.c_str());
                RegCloseKey(key);
            }
        }
    }
    return true;
}

bool is_implicit_layer_registered(const std::filesystem::path& manifest_path) {
    std::error_code ec;
    const std::wstring abs_path =
        std::filesystem::absolute(manifest_path, ec).lexically_normal().wstring();
    if (abs_path.empty()) return false;

    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS && key != nullptr) {
            DWORD type = 0;
            DWORD val = 0;
            DWORD size = sizeof(val);
            const LSTATUS st = RegQueryValueExW(key, abs_path.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(&val), &size);
            RegCloseKey(key);
            if (st == ERROR_SUCCESS && type == REG_DWORD && val == 0) {
                return true;
            }
        }
    }
    return false;
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

    // Per-game activation only: NEVER register the manifest in the global ApiLayers\Implicit
    // registry (it would load ReproVR into every OpenXR app). The drop-in proxy below sets
    // XR_API_LAYER_PATH / XR_ENABLE_API_LAYERS inside the game process only. Clean up any
    // global entry left by older installs for this manifest.
    unregister_implicit_layer(game_directory / kLayerManifest);

    // Install proxy DLL if present in source bundle, with fallback candidate selection
    std::filesystem::path source_proxy;
    for (const wchar_t* p : {L"version.dll", L"ReproVR_version_proxy.dll", L"ReproVR_proxy.dll"}) {
        if (std::filesystem::exists(source / p, code) && !code) {
            source_proxy = source / p;
            break;
        }
    }

    if (!source_proxy.empty()) {
        const auto marker_path = game_directory / kInstallMarkerName;
        const std::wstring previous = read_marker_proxy(marker_path);

        // A name is usable if it is free, or already holds our own proxy (never
        // overwrite a foreign version.dll / dxgi.dll from UEVR, ReShade, Cheeky...).
        auto usable = [&](const wchar_t* cand) {
            const auto p = game_directory / cand;
            return !std::filesystem::exists(p, code) || is_our_proxy(p);
        };
        // Prefer the name the game really loads; keep the previous choice when it is
        // as good as any other, so a re-install does not move files around.
        std::wstring chosen_proxy;
        int best = -1;
        for (const wchar_t* cand : kProxyCandidates) {
            if (!usable(cand)) continue;
            int score = proxy_load_evidence(game_directory, cand);
            if (previous == cand) score = score * 2 + 1;  // tie-break in favour of the existing one
            else score = score * 2;
            if (score > best) {
                best = score;
                chosen_proxy = cand;
            }
        }

        if (!chosen_proxy.empty()) {
            const auto target_proxy = game_directory / chosen_proxy;
            if (!copy_if_needed(source_proxy, target_proxy, error)) {
                return false;
            }
            // Moved to a better name: remove the previous proxy, but only if it is ours.
            if (!previous.empty() && previous != chosen_proxy) {
                const auto old_proxy = game_directory / previous;
                if (std::filesystem::exists(old_proxy, code) && is_our_proxy(old_proxy)) {
                    std::filesystem::remove(old_proxy, code);
                }
            }
            std::string narrow_proxy;
            for (const wchar_t ch : chosen_proxy) narrow_proxy += static_cast<char>(ch);
            std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
            marker << "ReproVR Installed\nproxy=" << narrow_proxy << "\n";
        }
    }

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

    // Unregister OpenXR manifest from HKCU/HKLM registry hives
    unregister_implicit_layer(game_directory / kLayerManifest);

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

    const auto marker_path = game_directory / kInstallMarkerName;
    if (std::filesystem::exists(marker_path, code) && !code) {
        std::ifstream mf(marker_path);
        std::string line;
        std::wstring proxy_to_remove;
        while (std::getline(mf, line)) {
            if (line.rfind("proxy=", 0) == 0 && line.size() > 6) {
                proxy_to_remove = std::wstring(line.begin() + 6, line.end());
                break;
            }
        }
        if (!proxy_to_remove.empty()) {
            std::filesystem::remove(game_directory / proxy_to_remove, code);
        }
        std::filesystem::remove(marker_path, code);
    }

    // repro_vr.ini holds the user's settings: uninstalling leaves it alone.
    return true;
}

bool proxy_will_load(const std::filesystem::path& game_directory) {
    const std::wstring proxy = read_marker_proxy(game_directory / kInstallMarkerName);
    // 2 or more = the game process itself reaches the proxy (exe or its import chain).
    return !proxy.empty() && proxy_load_evidence(game_directory, proxy.c_str()) >= 2;
}

// --- Legacy migration ---
// Older builds registered ReproVR GLOBALLY (OpenXR ApiLayers\Implicit). Rules of the
// migration, so that it can never leave a user's PC worse than it found it:
//   1. Only entries whose manifest FILE NAME or CONTENT names ReproVR are touched.
//   2. A known game is re-armed per game FIRST; if that fails (game running, locked
//      file...) its global entry is KEPT and reported, so nothing stops working.
//   3. Every entry is written to a backup file BEFORE deletion (--restore-legacy).
//   4. Both registry views (64-bit and 32-bit/WOW6432Node) are scanned.
//   5. --dry-run changes nothing; everything else is logged to migration.log.

namespace {

constexpr wchar_t kImplicitKey[] =
    L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";

std::wstring lower_ws(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

bool mentions_reprovr(const std::wstring& lowered) {
    return lowered.find(L"reprovr") != std::wstring::npos ||
           lowered.find(L"xrframebridge") != std::wstring::npos;
}

// A manifest is ours if its file name or its content names ReproVR / XRFrameBridge.
// Never the folder: a foreign layer living in a folder called "reprovr" stays alone.
bool is_reprovr_manifest(const std::wstring& value_name) {
    const std::filesystem::path p(value_name);
    if (mentions_reprovr(lower_ws(p.filename().wstring()))) return true;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string text(65536, ' ');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(f.gcount()));
    for (auto& ch : text) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return text.find("reprovr") != std::string::npos ||
           text.find("xrframebridge") != std::string::npos;
}

std::filesystem::path game_dir_of_manifest(const std::filesystem::path& manifest) {
    auto dir = manifest.parent_path();
    if (lower_ws(dir.filename().wstring()) == L"reprovr") dir = dir.parent_path();
    return dir;
}

std::wstring norm_dir(const std::filesystem::path& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    return lower_ws((ec ? p : c).lexically_normal().wstring());
}

// An unknown game folder that already holds a ReproVR layer DLL (next to the manifest or in
// its "reprovr" sub-folder) was clearly set up with ReproVR: re-arming it per game loses nothing.
bool has_reprovr_layer_dll(const std::filesystem::path& manifest) {
    for (const auto& folder : {manifest.parent_path(), game_dir_of_manifest(manifest)}) {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) continue;
        for (const auto& f : std::filesystem::directory_iterator(folder, ec)) {
            const std::wstring n = lower_ws(f.path().filename().wstring());
            if (n.rfind(L"xr_apilayer", 0) == 0 && mentions_reprovr(n) &&
                f.path().extension() == L".dll") {
                return true;
            }
        }
    }
    return false;
}

std::wstring subkey_of(const LegacyOptions& o) {
    return o.implicit_subkey.empty() ? std::wstring(kImplicitKey) : o.implicit_subkey;
}

std::filesystem::path default_store_dir() {
    return appdata_directory() / kStoreFolder;
}

std::filesystem::path backup_path_of(const LegacyOptions& o) {
    return o.backup_file.empty() ? default_store_dir() / L"legacy_global_backup.ini"
                                 : o.backup_file;
}

std::filesystem::path log_path_of(const LegacyOptions& o) {
    return o.log_file.empty() ? default_store_dir() / L"migration.log" : o.log_file;
}

void log_line(const LegacyOptions& o, LegacyRepairReport& r, const std::wstring& line) {
    r.log.push_back(line);
    if (o.dry_run) return;
    std::error_code ec;
    const auto path = log_path_of(o);
    std::filesystem::create_directories(path.parent_path(), ec);
    std::wofstream(path, std::ios::app) << line << L"\n";
}

// Appends one entry to the backup ini before it is deleted. False = not saved.
bool backup_entry(const LegacyOptions& o, const LegacyGlobalLayer& e, DWORD data) {
    const auto file = backup_path_of(o);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::wstring section = L"entry:" + e.manifest.wstring();
    return WritePrivateProfileStringW(section.c_str(), L"hive",
                                      e.machine_wide ? L"HKLM" : L"HKCU", file.c_str()) &&
           WritePrivateProfileStringW(section.c_str(), L"view32",
                                      e.view32 ? L"1" : L"0", file.c_str()) &&
           WritePrivateProfileStringW(section.c_str(), L"subkey", subkey_of(o).c_str(),
                                      file.c_str()) &&
           WritePrivateProfileStringW(section.c_str(), L"data",
                                      std::to_wstring(data).c_str(), file.c_str());
}

REGSAM view_flag(bool view32) { return view32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY; }

}  // namespace

std::vector<LegacyGlobalLayer> find_legacy_global_layers(const LegacyOptions& options) {
    std::vector<LegacyGlobalLayer> found;
    const std::wstring subkey = subkey_of(options);
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (root == HKEY_LOCAL_MACHINE && !options.include_machine) continue;
        for (bool view32 : {false, true}) {
            // Measured: HKCU\SOFTWARE is shared by both views (each entry seen twice),
            // only HKLM\SOFTWARE is redirected to WOW6432Node.
            if (view32 && root == HKEY_CURRENT_USER) continue;
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, subkey.c_str(), 0,
                              KEY_QUERY_VALUE | view_flag(view32),
                              &key) != ERROR_SUCCESS) {
                continue;
            }
            for (DWORD i = 0;; ++i) {
                wchar_t name[32768];
                DWORD len = static_cast<DWORD>(std::size(name));
                if (RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr,
                                  nullptr) != ERROR_SUCCESS) {
                    break;
                }
                const std::wstring value(name, len);
                if (is_reprovr_manifest(value)) {
                    found.push_back({root == HKEY_LOCAL_MACHINE, view32,
                                     std::filesystem::path(value)});
                }
            }
            RegCloseKey(key);
        }
    }
    return found;
}

bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = sizeof(e);
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) &&
                    e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

LegacyRepairReport repair_legacy_global_layers(const std::filesystem::path& source,
                                               const LegacyOptions& options) {
    LegacyRepairReport report;
    const auto entries = find_legacy_global_layers(options);
    report.found = static_cast<int>(entries.size());
    const auto games = load_games();
    const std::wstring subkey = subkey_of(options);

    for (const auto& entry : entries) {
        const std::wstring hive = entry.machine_wide ? L"HKLM" : L"HKCU";
        const std::wstring tag = hive + (entry.view32 ? L"[32] " : L"[64] ") +
                                 entry.manifest.wstring();
        const auto dir = game_dir_of_manifest(entry.manifest);

        bool known = entry.manifest.filename() == kLayerManifest ||
                     has_reprovr_layer_dll(entry.manifest);
        for (const auto& g : games) {
            known = known || norm_dir(g.directory) == norm_dir(dir);
        }
        std::error_code ec;
        const bool dir_ok = std::filesystem::is_directory(dir, ec);

        if (options.dry_run) {
            log_line(options, report,
                     L"[dry-run] " + tag +
                         (known && dir_ok ? L" -> re-arm per game, then remove"
                                          : L" -> remove (no ReproVR DLL in that folder: add the game in the list)"));
            continue;
        }

        // (2) Re-arm per game BEFORE touching the registry. A failure keeps the entry.
        if (known && dir_ok && !source.empty()) {
            std::wstring err;
            if (!install(dir, source, &err)) {
                report.kept.push_back(entry.manifest);
                log_line(options, report,
                         L"KEPT (per-game install failed: " + err + L") " + tag);
                continue;
            }
            ++report.reinstalled;
            // Nothing in the folder shows the game loads the proxy: removing the global
            // entry could silently turn ReproVR off in that game. Keep it for now.
            if (!proxy_will_load(dir)) {
                report.kept.push_back(entry.manifest);
                report.unverified.push_back(dir);
                log_line(options, report,
                         L"KEPT (game may not load the per-game proxy) " + tag);
                continue;
            }
        }

        // (3) Back up, then delete. No backup = no deletion.
        HKEY key = nullptr;
        const HKEY root = entry.machine_wide ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        LSTATUS st = RegOpenKeyExW(root, subkey.c_str(), 0,
                                   KEY_QUERY_VALUE | KEY_SET_VALUE | view_flag(entry.view32),
                                   &key);
        if (st != ERROR_SUCCESS) {
            // Non-elevated HKLM: cannot even open for writing.
            report.needs_admin.push_back(entry.manifest);
            log_line(options, report, L"NEEDS ADMIN " + tag);
            continue;
        }
        DWORD data = 0, type = 0, size = sizeof(data);
        RegQueryValueExW(key, entry.manifest.c_str(), nullptr, &type,
                         reinterpret_cast<LPBYTE>(&data), &size);
        if (!backup_entry(options, entry, data)) {
            RegCloseKey(key);
            report.kept.push_back(entry.manifest);
            log_line(options, report, L"KEPT (backup could not be written) " + tag);
            continue;
        }
        st = RegDeleteValueW(key, entry.manifest.c_str());
        RegCloseKey(key);
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
            report.needs_admin.push_back(entry.manifest);
            log_line(options, report, L"NEEDS ADMIN (delete refused) " + tag);
            continue;
        }
        ++report.removed;
        if (!(known && dir_ok)) report.not_rearmed.push_back(dir);
        log_line(options, report, L"REMOVED " + tag);
    }
    return report;
}

int restore_legacy_global_layers(const LegacyOptions& options) {
    const auto file = backup_path_of(options);
    wchar_t sections[65536]{};
    const DWORD n = GetPrivateProfileSectionNamesW(sections,
                                                   static_cast<DWORD>(std::size(sections)),
                                                   file.c_str());
    int restored = 0;
    for (const wchar_t* s = sections; n > 0 && *s != L'\0'; s += wcslen(s) + 1) {
        const std::wstring section = s;
        if (section.rfind(L"entry:", 0) != 0) continue;
        const std::wstring manifest = section.substr(6);
        const bool hklm = read_string(section.c_str(), L"hive", file) == L"HKLM";
        const bool view32 = read_string(section.c_str(), L"view32", file) == L"1";
        std::wstring subkey = read_string(section.c_str(), L"subkey", file);
        if (subkey.empty()) subkey = kImplicitKey;
        const DWORD data = static_cast<DWORD>(
            _wtoi(read_string(section.c_str(), L"data", file).c_str()));
        HKEY key = nullptr;
        if (RegCreateKeyExW(hklm ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, subkey.c_str(),
                            0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE | view_flag(view32), nullptr, &key,
                            nullptr) != ERROR_SUCCESS) {
            continue;
        }
        if (RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&data),
                           sizeof(data)) == ERROR_SUCCESS) {
            ++restored;
        }
        RegCloseKey(key);
    }
    return restored;
}

}  // namespace reprovr::setup
