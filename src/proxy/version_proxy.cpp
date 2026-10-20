#include "xrfg/proxy_bootstrap.hpp"

#include <windows.h>

#include <array>
#include <fstream>
#include <mutex>
#include <string>

namespace xrfg {
namespace {

constexpr const wchar_t* kLayerDllName = L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
constexpr const wchar_t* kVulkanLayerDllName = L"OFXR_vulkan_queue_layer.dll";
constexpr const wchar_t* kConfigIniName = L"ofxr_bridge.ini";
constexpr const wchar_t* kManifestJsonName = L"XR_APILAYER_XRFrameBridge_diagnostic.json";
constexpr const wchar_t* kInstallMarkerName = L"repro_vr_installed.manifest";
constexpr const wchar_t* kLayerName = L"XR_APILAYER_XRFrameBridge_diagnostic";

std::string escape_json_string(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (const char ch : input) {
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"') {
            out += "\\\"";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

bool register_explicit_layer_manifest_in_hive(
    HKEY root_hive,
    const std::filesystem::path& manifest_path) noexcept {
    try {
        std::error_code ec;
        const auto abs_manifest =
            std::filesystem::absolute(manifest_path, ec).lexically_normal();
        const auto temp_dir =
            std::filesystem::temp_directory_path(ec).lexically_normal().wstring();
        const std::wstring value_name = abs_manifest.wstring();
        if (!temp_dir.empty() && value_name.size() >= temp_dir.size() &&
            _wcsnicmp(value_name.data(), temp_dir.data(), temp_dir.size()) == 0) {
            return true;
        }
        HKEY key = nullptr;
        const LSTATUS create_status = RegCreateKeyExW(
            root_hive,
            L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit",
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            nullptr,
            &key,
            nullptr);
        if (create_status != ERROR_SUCCESS || key == nullptr) {
            return false;
        }
        constexpr DWORD enabled_value = 0;
        const LSTATUS set_status = RegSetValueExW(
            key,
            value_name.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled_value),
            sizeof(enabled_value));
        RegCloseKey(key);
        return set_status == ERROR_SUCCESS;
    } catch (...) {
        return false;
    }
}

void unregister_explicit_layer_manifest_in_hive(
    HKEY root_hive,
    const std::filesystem::path& manifest_path) noexcept {
    try {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                root_hive,
                L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit",
                0,
                KEY_SET_VALUE | KEY_WOW64_64KEY,
                &key) != ERROR_SUCCESS ||
            key == nullptr) {
            return;
        }
        const std::wstring value_name =
            std::filesystem::absolute(manifest_path).lexically_normal().wstring();
        static_cast<void>(RegDeleteValueW(key, value_name.c_str()));
        RegCloseKey(key);
    } catch (...) {
    }
}

}  // namespace

bool write_openxr_layer_manifest_json(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& layer_dll_path) noexcept {
    try {
        if (manifest_path.empty() || layer_dll_path.empty()) {
            return false;
        }
        std::error_code ec;
        if (manifest_path.has_parent_path()) {
            std::filesystem::create_directories(manifest_path.parent_path(), ec);
        }
        const std::string escaped_path =
            escape_json_string(wide_to_utf8(layer_dll_path.wstring()));
        if (escaped_path.empty()) {
            return false;
        }
        std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << "{\n"
            << "  \"file_format_version\": \"1.0.0\",\n"
            << "  \"api_layer\": {\n"
            << "    \"name\": \"XR_APILAYER_XRFrameBridge_diagnostic\",\n"
            << "    \"library_path\": \"" << escaped_path << "\",\n"
            << "    \"api_version\": \"1.0\",\n"
            << "    \"implementation_version\": \"1\",\n"
            << "    \"description\": \"ReproVR 6-DOF Depth & Optical Flow Frame Synthesis Layer\",\n"
            << "    \"disable_environment\": \"XRFG_DISABLE_OFXR_BRIDGE\"\n"
            << "  }\n"
            << "}\n";
        out.flush();
        return out.good();
    } catch (...) {
        return false;
    }
}

ProxyBootstrapResult bootstrap_repro_vr_from_directory(
    const std::filesystem::path& base_directory) noexcept {
    ProxyBootstrapResult result{};
    try {
        std::error_code ec;
        if (base_directory.empty() ||
            !std::filesystem::exists(base_directory, ec)) {
            return result;
        }

        const std::array<std::filesystem::path, 2> candidate_dirs{
            base_directory,
            base_directory / L"ofxr",
        };
        for (const auto& dir : candidate_dirs) {
            const auto candidate_dll = dir / kLayerDllName;
            if (std::filesystem::exists(candidate_dll, ec)) {
                result.layer_dll_found = true;
                result.layer_dll_path = std::filesystem::absolute(candidate_dll, ec);
                result.manifest_path = std::filesystem::absolute(dir / kManifestJsonName, ec);
                break;
            }
        }

        if (!result.layer_dll_found) {
            return result;
        }

        // Always regenerate the JSON manifest so moved folders or corrupted escapes are healed.
        result.manifest_ready = write_openxr_layer_manifest_json(
            result.manifest_path,
            result.layer_dll_path);
        if (!result.manifest_ready) {
            return result;
        }

        // Register in Explicit ApiLayers (HKCU and HKLM when elevated) so High-Integrity
        // processes (where Khronos loader ignores XR_API_LAYER_PATH) still resolve the layer
        // when XR_ENABLE_API_LAYERS is set in-process.
        static_cast<void>(register_explicit_layer_manifest_in_hive(
            HKEY_CURRENT_USER, result.manifest_path));
        static_cast<void>(register_explicit_layer_manifest_in_hive(
            HKEY_LOCAL_MACHINE, result.manifest_path));

        const std::wstring manifest_dir =
            result.manifest_path.parent_path().wstring();

        std::array<wchar_t, 4096> existing_path{};
        const DWORD path_len = GetEnvironmentVariableW(
            L"XR_API_LAYER_PATH",
            existing_path.data(),
            static_cast<DWORD>(existing_path.size()));
        std::wstring new_layer_path = manifest_dir;
        if (path_len > 0 && path_len < existing_path.size()) {
            const std::wstring current(existing_path.data(), path_len);
            if (current.find(manifest_dir) == std::wstring::npos) {
                new_layer_path = manifest_dir + L";" + current;
            } else {
                new_layer_path = current;
            }
        }
        const BOOL set_path_ok =
            SetEnvironmentVariableW(L"XR_API_LAYER_PATH", new_layer_path.c_str());

        std::array<wchar_t, 2048> existing_layers{};
        const DWORD layers_len = GetEnvironmentVariableW(
            L"XR_ENABLE_API_LAYERS",
            existing_layers.data(),
            static_cast<DWORD>(existing_layers.size()));
        std::wstring new_enabled_layers = kLayerName;
        if (layers_len > 0 && layers_len < existing_layers.size()) {
            const std::wstring current(existing_layers.data(), layers_len);
            if (current.find(kLayerName) == std::wstring::npos) {
                new_enabled_layers = std::wstring(kLayerName) + L";" + current;
            } else {
                new_enabled_layers = current;
            }
        }
        const BOOL set_enable_ok = SetEnvironmentVariableW(
            L"XR_ENABLE_API_LAYERS",
            new_enabled_layers.c_str());

        result.environment_configured =
            (set_path_ok != FALSE) && (set_enable_ok != FALSE);
        return result;
    } catch (...) {
        return result;
    }
}

GameDropInInstallResult install_dropin_to_game_directory(
    const std::filesystem::path& source_bundle_dir,
    const std::filesystem::path& target_game_dir) noexcept {
    GameDropInInstallResult result{};
    result.game_directory = target_game_dir;
    try {
        std::error_code ec;
        if (source_bundle_dir.empty() || target_game_dir.empty()) {
            result.error_message = L"Source bundle or target game directory is empty.";
            return result;
        }
        std::filesystem::create_directories(target_game_dir, ec);
        if (ec) {
            result.error_message = L"Failed to create target game directory.";
            return result;
        }

        // Refuse to overwrite a foreign version.dll unless our marker file is present.
        const auto target_proxy = target_game_dir / L"version.dll";
        const auto marker_path = target_game_dir / kInstallMarkerName;
        if (std::filesystem::exists(target_proxy, ec) &&
            !std::filesystem::exists(marker_path, ec)) {
            result.error_message =
                L"Target directory already contains a third-party version.dll.";
            return result;
        }

        // Locate source files in source_bundle_dir or source_bundle_dir / "ofxr".
        auto find_source = [&](const wchar_t* filename) -> std::filesystem::path {
            const auto direct = source_bundle_dir / filename;
            if (std::filesystem::exists(direct, ec)) {
                return direct;
            }
            const auto sub = source_bundle_dir / L"ofxr" / filename;
            if (std::filesystem::exists(sub, ec)) {
                return sub;
            }
            return {};
        };

        const auto src_layer = find_source(kLayerDllName);
        if (src_layer.empty()) {
            result.error_message =
                L"Could not find XR_APILAYER_XRFrameBridge_diagnostic.dll in bundle.";
            return result;
        }

        auto src_proxy = find_source(L"version.dll");
        if (src_proxy.empty()) {
            src_proxy = find_source(L"ReproVR_version_proxy.dll");
        }

        const auto dst_layer = target_game_dir / kLayerDllName;
        std::filesystem::copy_file(
            src_layer,
            dst_layer,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        if (ec) {
            result.error_message = L"Failed to copy OpenXR layer DLL to game folder.";
            return result;
        }
        result.installed_layer_dll = dst_layer;

        if (!src_proxy.empty()) {
            std::filesystem::copy_file(
                src_proxy,
                target_proxy,
                std::filesystem::copy_options::overwrite_existing,
                ec);
            if (ec) {
                result.error_message = L"Failed to copy version.dll proxy to game folder.";
                return result;
            }
            result.installed_proxy_dll = target_proxy;
        }

        const auto src_vk = find_source(kVulkanLayerDllName);
        if (!src_vk.empty()) {
            std::filesystem::copy_file(
                src_vk,
                target_game_dir / kVulkanLayerDllName,
                std::filesystem::copy_options::overwrite_existing,
                ec);
        }

        const auto src_ini = find_source(kConfigIniName);
        if (!src_ini.empty()) {
            std::filesystem::copy_file(
                src_ini,
                target_game_dir / kConfigIniName,
                std::filesystem::copy_options::overwrite_existing,
                ec);
        }

        const auto dst_manifest = target_game_dir / kManifestJsonName;
        if (!write_openxr_layer_manifest_json(
                dst_manifest,
                std::filesystem::absolute(dst_layer, ec))) {
            result.error_message = L"Failed to write OpenXR layer JSON manifest.";
            return result;
        }
        result.installed_manifest_json = dst_manifest;

        std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
        marker << "ReproVR Drop-In Installation v1.0\n";
        marker.close();

        result.succeeded = true;
        return result;
    } catch (...) {
        result.error_message = L"Unexpected exception during drop-in installation.";
        return result;
    }
}

bool remove_dropin_from_game_directory(
    const std::filesystem::path& target_game_dir) noexcept {
    try {
        std::error_code ec;
        if (target_game_dir.empty() ||
            !std::filesystem::exists(target_game_dir, ec)) {
            return false;
        }
        const auto marker_path = target_game_dir / kInstallMarkerName;
        if (!std::filesystem::exists(marker_path, ec)) {
            return false;
        }

        const auto manifest_abs =
            std::filesystem::absolute(target_game_dir / kManifestJsonName, ec);
        unregister_explicit_layer_manifest_in_hive(HKEY_CURRENT_USER, manifest_abs);
        unregister_explicit_layer_manifest_in_hive(HKEY_LOCAL_MACHINE, manifest_abs);

        const std::array<const wchar_t*, 6> managed_files{
            L"version.dll",
            kLayerDllName,
            kVulkanLayerDllName,
            kConfigIniName,
            kManifestJsonName,
            kInstallMarkerName,
        };
        for (const wchar_t* file : managed_files) {
            const auto path = target_game_dir / file;
            if (std::filesystem::exists(path, ec)) {
                std::filesystem::remove(path, ec);
            }
        }
        return !std::filesystem::exists(marker_path, ec);
    } catch (...) {
        return false;
    }
}

}  // namespace xrfg

namespace {

struct SystemVersionTable {
    std::once_flag once;
    HMODULE module{};
    xrfg::ProxyBootstrapResult bootstrap{};

    BOOL(WINAPI* pGetFileVersionInfoA)(LPCSTR, DWORD, DWORD, LPVOID){};
    BOOL(WINAPI* pGetFileVersionInfoByHandle)(DWORD, HANDLE, LPVOID*, PDWORD){};
    BOOL(WINAPI* pGetFileVersionInfoExA)(DWORD, LPCSTR, DWORD, DWORD, LPVOID){};
    BOOL(WINAPI* pGetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID){};
    DWORD(WINAPI* pGetFileVersionInfoSizeA)(LPCSTR, LPDWORD){};
    DWORD(WINAPI* pGetFileVersionInfoSizeExA)(DWORD, LPCSTR, LPDWORD){};
    DWORD(WINAPI* pGetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD){};
    DWORD(WINAPI* pGetFileVersionInfoSizeW)(LPCWSTR, LPDWORD){};
    BOOL(WINAPI* pGetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID){};
    DWORD(WINAPI* pVerFindFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT){};
    DWORD(WINAPI* pVerFindFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT){};
    DWORD(WINAPI* pVerInstallFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT){};
    DWORD(WINAPI* pVerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT){};
    DWORD(WINAPI* pVerLanguageNameA)(DWORD, LPSTR, DWORD){};
    DWORD(WINAPI* pVerLanguageNameW)(DWORD, LPWSTR, DWORD){};
    BOOL(WINAPI* pVerQueryValueA)(LPCVOID, LPCSTR, LPVOID*, PUINT){};
    BOOL(WINAPI* pVerQueryValueW)(LPCVOID, LPCWSTR, LPVOID*, PUINT){};
};

SystemVersionTable& version_table() {
    static SystemVersionTable table;
    return table;
}

HMODULE g_self_module = nullptr;

void ensure_version_initialized() noexcept {
    auto& t = version_table();
    std::call_once(t.once, [&]() noexcept {
        std::array<wchar_t, MAX_PATH> sys_dir{};
        const UINT len = GetSystemDirectoryW(
            sys_dir.data(),
            static_cast<UINT>(sys_dir.size()));
        if (len > 0 && len < sys_dir.size()) {
            const std::filesystem::path sys_version =
                std::filesystem::path(std::wstring(sys_dir.data(), len)) /
                L"version.dll";
            t.module = LoadLibraryExW(
                sys_version.c_str(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (t.module != nullptr) {
            t.bootstrap.system_version_loaded = true;
#define BIND_VER_PROC(name) \
    t.p##name = reinterpret_cast<decltype(t.p##name)>(GetProcAddress(t.module, #name))
            BIND_VER_PROC(GetFileVersionInfoA);
            BIND_VER_PROC(GetFileVersionInfoByHandle);
            BIND_VER_PROC(GetFileVersionInfoExA);
            BIND_VER_PROC(GetFileVersionInfoExW);
            BIND_VER_PROC(GetFileVersionInfoSizeA);
            BIND_VER_PROC(GetFileVersionInfoSizeExA);
            BIND_VER_PROC(GetFileVersionInfoSizeExW);
            BIND_VER_PROC(GetFileVersionInfoSizeW);
            BIND_VER_PROC(GetFileVersionInfoW);
            BIND_VER_PROC(VerFindFileA);
            BIND_VER_PROC(VerFindFileW);
            BIND_VER_PROC(VerInstallFileA);
            BIND_VER_PROC(VerInstallFileW);
            BIND_VER_PROC(VerLanguageNameA);
            BIND_VER_PROC(VerLanguageNameW);
            BIND_VER_PROC(VerQueryValueA);
            BIND_VER_PROC(VerQueryValueW);
#undef BIND_VER_PROC
        }

        std::array<wchar_t, MAX_PATH> self_path{};
        const DWORD self_len = GetModuleFileNameW(
            g_self_module,
            self_path.data(),
            static_cast<DWORD>(self_path.size()));
        if (self_len > 0 && self_len < self_path.size()) {
            const std::filesystem::path base_dir =
                std::filesystem::path(std::wstring(self_path.data(), self_len))
                    .parent_path();
            const auto boot = xrfg::bootstrap_repro_vr_from_directory(base_dir);
            t.bootstrap.layer_dll_found = boot.layer_dll_found;
            t.bootstrap.manifest_ready = boot.manifest_ready;
            t.bootstrap.environment_configured = boot.environment_configured;
            t.bootstrap.layer_dll_path = boot.layer_dll_path;
            t.bootstrap.manifest_path = boot.manifest_path;
        }
    });
}

}  // namespace

extern "C" {

BOOL WINAPI proxy_GetFileVersionInfoA(
    LPCSTR lptstrFilename,
    DWORD dwHandle,
    DWORD dwLen,
    LPVOID lpData) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoA;
    return fn ? fn(lptstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

BOOL WINAPI proxy_GetFileVersionInfoByHandle(
    DWORD dwFlags,
    HANDLE hFile,
    LPVOID* lplpData,
    PDWORD pdwLen) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoByHandle;
    return fn ? fn(dwFlags, hFile, lplpData, pdwLen) : FALSE;
}

BOOL WINAPI proxy_GetFileVersionInfoExA(
    DWORD dwFlags,
    LPCSTR lpwstrFilename,
    DWORD dwHandle,
    DWORD dwLen,
    LPVOID lpData) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoExA;
    return fn ? fn(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

BOOL WINAPI proxy_GetFileVersionInfoExW(
    DWORD dwFlags,
    LPCWSTR lpwstrFilename,
    DWORD dwHandle,
    DWORD dwLen,
    LPVOID lpData) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoExW;
    return fn ? fn(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

DWORD WINAPI proxy_GetFileVersionInfoSizeA(
    LPCSTR lptstrFilename,
    LPDWORD lpdwHandle) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoSizeA;
    return fn ? fn(lptstrFilename, lpdwHandle) : 0U;
}

DWORD WINAPI proxy_GetFileVersionInfoSizeExA(
    DWORD dwFlags,
    LPCSTR lpwstrFilename,
    LPDWORD lpdwHandle) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoSizeExA;
    return fn ? fn(dwFlags, lpwstrFilename, lpdwHandle) : 0U;
}

DWORD WINAPI proxy_GetFileVersionInfoSizeExW(
    DWORD dwFlags,
    LPCWSTR lpwstrFilename,
    LPDWORD lpdwHandle) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoSizeExW;
    return fn ? fn(dwFlags, lpwstrFilename, lpdwHandle) : 0U;
}

DWORD WINAPI proxy_GetFileVersionInfoSizeW(
    LPCWSTR lptstrFilename,
    LPDWORD lpdwHandle) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoSizeW;
    return fn ? fn(lptstrFilename, lpdwHandle) : 0U;
}

BOOL WINAPI proxy_GetFileVersionInfoW(
    LPCWSTR lptstrFilename,
    DWORD dwHandle,
    DWORD dwLen,
    LPVOID lpData) {
    ensure_version_initialized();
    auto fn = version_table().pGetFileVersionInfoW;
    return fn ? fn(lptstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

DWORD WINAPI proxy_VerFindFileA(
    DWORD uFlags,
    LPCSTR szFileName,
    LPCSTR szWinDir,
    LPCSTR szAppDir,
    LPSTR szCurDir,
    PUINT lpuCurDirLen,
    LPSTR szDestDir,
    PUINT lpuDestDirLen) {
    ensure_version_initialized();
    auto fn = version_table().pVerFindFileA;
    return fn
        ? fn(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen)
        : 0U;
}

DWORD WINAPI proxy_VerFindFileW(
    DWORD uFlags,
    LPCWSTR szFileName,
    LPCWSTR szWinDir,
    LPCWSTR szAppDir,
    LPWSTR szCurDir,
    PUINT lpuCurDirLen,
    LPWSTR szDestDir,
    PUINT lpuDestDirLen) {
    ensure_version_initialized();
    auto fn = version_table().pVerFindFileW;
    return fn
        ? fn(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen)
        : 0U;
}

DWORD WINAPI proxy_VerInstallFileA(
    DWORD uFlags,
    LPCSTR szSrcFileName,
    LPCSTR szDestFileName,
    LPCSTR szSrcDir,
    LPCSTR szDestDir,
    LPCSTR szCurDir,
    LPSTR szTmpFile,
    PUINT lpuTmpFileLen) {
    ensure_version_initialized();
    auto fn = version_table().pVerInstallFileA;
    return fn
        ? fn(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen)
        : 0U;
}

DWORD WINAPI proxy_VerInstallFileW(
    DWORD uFlags,
    LPCWSTR szSrcFileName,
    LPCWSTR szDestFileName,
    LPCWSTR szSrcDir,
    LPCWSTR szDestDir,
    LPCWSTR szCurDir,
    LPWSTR szTmpFile,
    PUINT lpuTmpFileLen) {
    ensure_version_initialized();
    auto fn = version_table().pVerInstallFileW;
    return fn
        ? fn(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen)
        : 0U;
}

DWORD WINAPI proxy_VerLanguageNameA(
    DWORD wLang,
    LPSTR szLang,
    DWORD cchLang) {
    ensure_version_initialized();
    auto fn = version_table().pVerLanguageNameA;
    return fn ? fn(wLang, szLang, cchLang) : 0U;
}

DWORD WINAPI proxy_VerLanguageNameW(
    DWORD wLang,
    LPWSTR szLang,
    DWORD cchLang) {
    ensure_version_initialized();
    auto fn = version_table().pVerLanguageNameW;
    return fn ? fn(wLang, szLang, cchLang) : 0U;
}

BOOL WINAPI proxy_VerQueryValueA(
    LPCVOID pBlock,
    LPCSTR lpSubBlock,
    LPVOID* lplpBuffer,
    PUINT puLen) {
    ensure_version_initialized();
    auto fn = version_table().pVerQueryValueA;
    return fn ? fn(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE;
}

BOOL WINAPI proxy_VerQueryValueW(
    LPCVOID pBlock,
    LPCWSTR lpSubBlock,
    LPVOID* lplpBuffer,
    PUINT puLen) {
    ensure_version_initialized();
    auto fn = version_table().pVerQueryValueW;
    return fn ? fn(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE;
}

std::uint32_t WINAPI ReproVR_GetProxyBootstrapStatus() {
    ensure_version_initialized();
    const auto& b = version_table().bootstrap;
    std::uint32_t mask = 0U;
    if (b.system_version_loaded) mask |= 1U << 0;
    if (b.layer_dll_found) mask |= 1U << 1;
    if (b.manifest_ready) mask |= 1U << 2;
    if (b.environment_configured) mask |= 1U << 3;
    return mask;
}

}  // extern "C"

#ifdef XRFG_BUILDING_VERSION_PROXY_DLL
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_self_module = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
#endif
