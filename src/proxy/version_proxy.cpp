#include "xrfg/proxy_bootstrap.hpp"

#include <windows.h>

#include <array>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace xrfg {
namespace {

constexpr const wchar_t* kLayerDllName = L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
constexpr const wchar_t* kVulkanLayerDllName = L"OFXR_vulkan_queue_layer.dll";
constexpr const wchar_t* kConfigIniName = L"repro_vr.ini";
constexpr const wchar_t* kLegacyConfigIniName = L"ofxr_bridge.ini";
constexpr const wchar_t* kManifestJsonName = L"XR_APILAYER_XRFrameBridge_diagnostic.json";
constexpr const wchar_t* kInstallMarkerName = L"repro_vr_installed.manifest";
constexpr const wchar_t* kLayerName = L"XR_APILAYER_XRFrameBridge_diagnostic";

constexpr std::array<const wchar_t*, 3> kProxyCandidates = {
    L"version.dll",
    L"winmm.dll",
    L"dxgi.dll",
};

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

        bool ok = false;
        constexpr DWORD enabled_value = 0;
        for (const wchar_t* subkey : {
                 L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
                 L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit"}) {
            HKEY key = nullptr;
            const LSTATUS create_status = RegCreateKeyExW(
                root_hive,
                subkey,
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
                nullptr,
                &key,
                nullptr);
            if (create_status == ERROR_SUCCESS && key != nullptr) {
                const LSTATUS set_status = RegSetValueExW(
                    key,
                    value_name.c_str(),
                    0,
                    REG_DWORD,
                    reinterpret_cast<const BYTE*>(&enabled_value),
                    sizeof(enabled_value));
                RegCloseKey(key);
                if (set_status == ERROR_SUCCESS) {
                    ok = true;
                }
            }
        }
        return ok;
    } catch (...) {
        return false;
    }
}

void unregister_explicit_layer_manifest_in_hive(
    HKEY root_hive,
    const std::filesystem::path& manifest_path) noexcept {
    try {
        const std::wstring value_name =
            std::filesystem::absolute(manifest_path).lexically_normal().wstring();
        for (const wchar_t* subkey : {
                 L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
                 L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit"}) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(
                    root_hive,
                    subkey,
                    0,
                    KEY_SET_VALUE | KEY_WOW64_64KEY,
                    &key) == ERROR_SUCCESS &&
                key != nullptr) {
                static_cast<void>(RegDeleteValueW(key, value_name.c_str()));
                RegCloseKey(key);
            }
        }
    } catch (...) {
    }
}

}  // namespace

bool is_repro_vr_proxy(
    const std::filesystem::path& target_game_dir,
    const std::wstring& proxy_filename) noexcept {
    try {
        std::error_code ec;
        if (target_game_dir.empty() || proxy_filename.empty()) {
            return false;
        }
        const auto proxy_path = target_game_dir / proxy_filename;
        if (!std::filesystem::exists(proxy_path, ec)) {
            return false;
        }

        // 1. Check if install marker records this proxy filename
        const auto marker_path = target_game_dir / kInstallMarkerName;
        if (std::filesystem::exists(marker_path, ec)) {
            std::ifstream mf(marker_path);
            std::string line;
            while (std::getline(mf, line)) {
                if (line.rfind("proxy=", 0) == 0 && line.size() > 6) {
                    std::wstring recorded(line.begin() + 6, line.end());
                    if (_wcsicmp(recorded.c_str(), proxy_filename.c_str()) == 0) {
                        return true;
                    }
                }
            }
        }

        // 2. Probe DLL export for our marker function
        HMODULE hProbe = LoadLibraryExW(
            proxy_path.c_str(),
            nullptr,
            DONT_RESOLVE_DLL_REFERENCES);
        if (hProbe != nullptr) {
            const bool has_marker =
                GetProcAddress(hProbe, "ReproVR_GetProxyBootstrapStatus") != nullptr;
            FreeLibrary(hProbe);
            if (has_marker) {
                return true;
            }
        }

        return false;
    } catch (...) {
        return false;
    }
}

std::wstring select_available_proxy_filename(
    const std::filesystem::path& target_game_dir) noexcept {
    try {
        std::error_code ec;
        if (target_game_dir.empty()) {
            return kProxyCandidates[0];
        }

        // 1. If our marker is already installed, keep the active proxy
        const auto marker_path = target_game_dir / kInstallMarkerName;
        if (std::filesystem::exists(marker_path, ec)) {
            std::ifstream mf(marker_path);
            std::string line;
            while (std::getline(mf, line)) {
                if (line.rfind("proxy=", 0) == 0 && line.size() > 6) {
                    std::wstring existing_proxy(line.begin() + 6, line.end());
                    if (std::filesystem::exists(target_game_dir / existing_proxy, ec)) {
                        return existing_proxy;
                    }
                }
            }
        }

        // 2. Search candidates in priority order (version.dll -> winmm.dll -> dxgi.dll)
        for (const wchar_t* cand : kProxyCandidates) {
            const auto cand_path = target_game_dir / cand;
            if (!std::filesystem::exists(cand_path, ec)) {
                return cand;
            }
            if (is_repro_vr_proxy(target_game_dir, cand)) {
                return cand;
            }
        }

        return {};
    } catch (...) {
        return kProxyCandidates[0];
    }
}

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

        result.manifest_ready = write_openxr_layer_manifest_json(
            result.manifest_path,
            result.layer_dll_path);
        if (!result.manifest_ready) {
            return result;
        }

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

        const std::wstring chosen_proxy = select_available_proxy_filename(target_game_dir);
        if (chosen_proxy.empty()) {
            result.error_message =
                L"Target directory already contains foreign versions of version.dll, winmm.dll, and dxgi.dll.";
            return result;
        }
        const auto target_proxy = target_game_dir / chosen_proxy;
        const auto marker_path = target_game_dir / kInstallMarkerName;

        auto find_source = [&](const wchar_t* filename) -> std::filesystem::path {
            const auto direct = source_bundle_dir / filename;
            if (std::filesystem::exists(direct, ec)) return direct;
            const auto sub_repro = source_bundle_dir / L"repro_vr" / filename;
            if (std::filesystem::exists(sub_repro, ec)) return sub_repro;
            const auto sub = source_bundle_dir / L"ofxr" / filename;
            if (std::filesystem::exists(sub, ec)) return sub;
            return {};
        };

        const auto src_layer = find_source(kLayerDllName);
        if (src_layer.empty()) {
            result.error_message =
                L"Could not find XR_APILAYER_XRFrameBridge_diagnostic.dll in bundle.";
            return result;
        }

        auto src_proxy = find_source(chosen_proxy.c_str());
        if (src_proxy.empty()) {
            src_proxy = find_source(L"version.dll");
        }
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
                result.error_message = L"Failed to copy proxy DLL to game folder.";
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

        auto src_ini = find_source(kConfigIniName);
        if (src_ini.empty()) {
            src_ini = find_source(kLegacyConfigIniName);
        }
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

        std::string narrow_proxy;
        for (const wchar_t ch : chosen_proxy) narrow_proxy += static_cast<char>(ch);
        std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
        marker << "ReproVR Drop-In Installation v1.0\nproxy="
               << narrow_proxy
               << "\n";
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

        std::wstring installed_proxy = L"version.dll";
        {
            std::ifstream mf(marker_path);
            std::string line;
            while (std::getline(mf, line)) {
                if (line.rfind("proxy=", 0) == 0 && line.size() > 6) {
                    installed_proxy = std::wstring(line.begin() + 6, line.end());
                    break;
                }
            }
        }

        const auto manifest_abs =
            std::filesystem::absolute(target_game_dir / kManifestJsonName, ec);
        unregister_explicit_layer_manifest_in_hive(HKEY_CURRENT_USER, manifest_abs);
        unregister_explicit_layer_manifest_in_hive(HKEY_LOCAL_MACHINE, manifest_abs);

        if (!installed_proxy.empty()) {
            std::filesystem::remove(target_game_dir / installed_proxy, ec);
        }

        const std::array<const wchar_t*, 6> managed_files{
            kLayerDllName,
            kVulkanLayerDllName,
            kConfigIniName,
            kLegacyConfigIniName,
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

struct SystemProxyState {
    std::once_flag once;
    HMODULE module{};
    xrfg::ProxyBootstrapResult bootstrap{};
    std::wstring self_name{L"version.dll"};
};

SystemProxyState& proxy_state() {
    static SystemProxyState state;
    return state;
}

HMODULE g_self_module = nullptr;

void ensure_proxy_initialized() noexcept {
    auto& s = proxy_state();
    std::call_once(s.once, [&]() noexcept {
        std::array<wchar_t, MAX_PATH> self_path{};
        const DWORD self_len = GetModuleFileNameW(
            g_self_module,
            self_path.data(),
            static_cast<DWORD>(self_path.size()));
        std::filesystem::path base_dir;
        if (self_len > 0 && self_len < self_path.size()) {
            std::filesystem::path p(std::wstring(self_path.data(), self_len));
            s.self_name = p.filename().wstring();
            base_dir = p.parent_path();
        }

        std::array<wchar_t, MAX_PATH> sys_dir{};
        const UINT len = GetSystemDirectoryW(
            sys_dir.data(),
            static_cast<UINT>(sys_dir.size()));
        if (len > 0 && len < sys_dir.size()) {
            const std::filesystem::path sys_path =
                std::filesystem::path(std::wstring(sys_dir.data(), len)) / s.self_name;
            s.module = LoadLibraryExW(
                sys_path.c_str(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH);
            if (s.module == nullptr) {
                const std::filesystem::path fallback =
                    std::filesystem::path(std::wstring(sys_dir.data(), len)) / L"version.dll";
                s.module = LoadLibraryExW(
                    fallback.c_str(),
                    nullptr,
                    LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }

        if (s.module != nullptr) {
            s.bootstrap.system_version_loaded = true;
        }

        if (!base_dir.empty()) {
            const auto boot = xrfg::bootstrap_repro_vr_from_directory(base_dir);
            s.bootstrap.layer_dll_found = boot.layer_dll_found;
            s.bootstrap.manifest_ready = boot.manifest_ready;
            s.bootstrap.environment_configured = boot.environment_configured;
            s.bootstrap.layer_dll_path = boot.layer_dll_path;
            s.bootstrap.manifest_path = boot.manifest_path;
        }
    });
}

UINT_PTR forward_system_proc(
    const char* name,
    UINT_PTR a1 = 0,
    UINT_PTR a2 = 0,
    UINT_PTR a3 = 0,
    UINT_PTR a4 = 0,
    UINT_PTR a5 = 0,
    UINT_PTR a6 = 0,
    UINT_PTR a7 = 0,
    UINT_PTR a8 = 0) noexcept {
    ensure_proxy_initialized();
    HMODULE mod = proxy_state().module;
    if (mod != nullptr) {
        FARPROC proc = GetProcAddress(mod, name);
        if (proc != nullptr) {
            using Fn = UINT_PTR(WINAPI*)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);
            return reinterpret_cast<Fn>(proc)(a1, a2, a3, a4, a5, a6, a7, a8);
        }
    }
    return 0;
}

}  // namespace

extern "C" {

std::uint32_t WINAPI ReproVR_GetProxyBootstrapStatus() {
    ensure_proxy_initialized();
    const auto& b = proxy_state().bootstrap;
    std::uint32_t mask = 0U;
    if (b.system_version_loaded) mask |= 1U << 0;
    if (b.layer_dll_found) mask |= 1U << 1;
    if (b.manifest_ready) mask |= 1U << 2;
    if (b.environment_configured) mask |= 1U << 3;
    return mask;
}

UINT_PTR WINAPI proxy_ApplyCompatResolutionQuirking(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("ApplyCompatResolutionQuirking", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_CloseDriver(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("CloseDriver", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_CompatString(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("CompatString", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_CompatValue(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("CompatValue", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_CreateDXGIFactory(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("CreateDXGIFactory", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_CreateDXGIFactory1(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("CreateDXGIFactory1", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_CreateDXGIFactory2(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("CreateDXGIFactory2", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGID3D10CreateDevice(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGID3D10CreateDevice", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGID3D10CreateLayeredDevice(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGID3D10CreateLayeredDevice", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGID3D10GetLayeredDeviceSize(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGID3D10GetLayeredDeviceSize", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGID3D10RegisterLayers(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGID3D10RegisterLayers", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGIDeclareAdapterRemovalSupport(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGIDeclareAdapterRemovalSupport", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGIDisableVBlankVirtualization(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGIDisableVBlankVirtualization", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGIDumpJournal(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGIDumpJournal", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGIGetDebugInterface1(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGIGetDebugInterface1", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DXGIReportAdapterConfiguration(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DXGIReportAdapterConfiguration", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DefDriverProc(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DefDriverProc", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DriverCallback(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DriverCallback", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_DrvGetModuleHandle(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("DrvGetModuleHandle", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetDriverModuleHandle(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetDriverModuleHandle", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoByHandle(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoByHandle", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoExA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoExA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoExW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoExW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoSizeA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoSizeA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoSizeExA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoSizeExA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoSizeExW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoSizeExW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoSizeW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoSizeW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_GetFileVersionInfoW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("GetFileVersionInfoW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_OpenDriver(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("OpenDriver", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_PIXBeginCapture(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("PIXBeginCapture", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_PIXEndCapture(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("PIXEndCapture", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_PIXGetCaptureState(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("PIXGetCaptureState", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_PlaySound(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("PlaySound", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_PlaySoundA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("PlaySoundA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_PlaySoundW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("PlaySoundW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_SendDriverMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("SendDriverMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_SetAppCompatStringPointer(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("SetAppCompatStringPointer", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_UpdateHMDEmulationStatus(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("UpdateHMDEmulationStatus", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerFindFileA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerFindFileA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerFindFileW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerFindFileW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerInstallFileA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerInstallFileA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerInstallFileW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerInstallFileW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerLanguageNameA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerLanguageNameA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerLanguageNameW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerLanguageNameW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerQueryValueA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerQueryValueA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_VerQueryValueW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("VerQueryValueW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_WOWAppExit(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("WOWAppExit", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_auxGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("auxGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_auxGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("auxGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_auxGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("auxGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_auxGetVolume(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("auxGetVolume", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_auxOutMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("auxOutMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_auxSetVolume(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("auxSetVolume", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyConfigChanged(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyConfigChanged", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyGetPos(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyGetPos", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyGetPosEx(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyGetPosEx", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyGetThreshold(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyGetThreshold", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joyReleaseCapture(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joyReleaseCapture", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joySetCapture(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joySetCapture", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_joySetThreshold(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("joySetThreshold", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciDriverNotify(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciDriverNotify", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciDriverYield(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciDriverYield", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciExecute(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciExecute", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciFreeCommandResource(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciFreeCommandResource", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetCreatorTask(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetCreatorTask", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetDeviceIDA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetDeviceIDA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetDeviceIDFromElementIDA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetDeviceIDFromElementIDA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetDeviceIDFromElementIDW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetDeviceIDFromElementIDW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetDeviceIDW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetDeviceIDW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetDriverData(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetDriverData", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetErrorStringA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetErrorStringA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetErrorStringW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetErrorStringW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciGetYieldProc(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciGetYieldProc", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciLoadCommandResource(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciLoadCommandResource", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciSendCommandA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciSendCommandA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciSendCommandW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciSendCommandW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciSendStringA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciSendStringA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciSendStringW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciSendStringW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciSetDriverData(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciSetDriverData", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mciSetYieldProc(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mciSetYieldProc", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiConnect(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiConnect", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiDisconnect(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiDisconnect", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInAddBuffer(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInAddBuffer", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInGetErrorTextA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInGetErrorTextA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInGetErrorTextW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInGetErrorTextW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInGetID(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInGetID", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInOpen(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInOpen", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInPrepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInPrepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInReset(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInReset", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInStart(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInStart", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInStop(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInStop", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiInUnprepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiInUnprepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutCacheDrumPatches(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutCacheDrumPatches", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutCachePatches(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutCachePatches", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetErrorTextA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetErrorTextA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetErrorTextW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetErrorTextW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetID(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetID", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutGetVolume(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutGetVolume", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutLongMsg(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutLongMsg", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutOpen(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutOpen", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutPrepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutPrepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutReset(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutReset", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutSetVolume(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutSetVolume", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutShortMsg(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutShortMsg", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiOutUnprepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiOutUnprepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamOpen(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamOpen", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamOut(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamOut", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamPause(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamPause", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamPosition(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamPosition", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamProperty(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamProperty", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamRestart(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamRestart", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_midiStreamStop(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("midiStreamStop", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetControlDetailsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetControlDetailsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetControlDetailsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetControlDetailsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetID(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetID", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetLineControlsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetLineControlsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetLineControlsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetLineControlsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetLineInfoA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetLineInfoA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetLineInfoW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetLineInfoW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerOpen(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerOpen", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mixerSetControlDetails(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mixerSetControlDetails", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmDrvInstall(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmDrvInstall", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmGetCurrentTask(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmGetCurrentTask", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmTaskBlock(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmTaskBlock", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmTaskCreate(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmTaskCreate", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmTaskSignal(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmTaskSignal", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmTaskYield(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmTaskYield", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioAdvance(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioAdvance", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioAscend(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioAscend", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioCreateChunk(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioCreateChunk", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioDescend(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioDescend", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioFlush(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioFlush", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioGetInfo(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioGetInfo", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioInstallIOProcA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioInstallIOProcA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioInstallIOProcW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioInstallIOProcW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioOpenA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioOpenA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioOpenW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioOpenW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioRead(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioRead", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioRenameA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioRenameA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioRenameW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioRenameW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioSeek(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioSeek", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioSendMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioSendMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioSetBuffer(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioSetBuffer", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioSetInfo(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioSetInfo", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioStringToFOURCCA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioStringToFOURCCA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioStringToFOURCCW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioStringToFOURCCW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmioWrite(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmioWrite", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_mmsystemGetVersion(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("mmsystemGetVersion", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_sndPlaySoundA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("sndPlaySoundA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_sndPlaySoundW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("sndPlaySoundW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeBeginPeriod(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeBeginPeriod", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeEndPeriod(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeEndPeriod", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeGetDevCaps(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeGetDevCaps", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeGetSystemTime(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeGetSystemTime", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeGetTime(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeGetTime", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeKillEvent(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeKillEvent", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_timeSetEvent(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("timeSetEvent", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInAddBuffer(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInAddBuffer", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetErrorTextA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetErrorTextA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetErrorTextW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetErrorTextW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetID(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetID", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInGetPosition(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInGetPosition", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInOpen(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInOpen", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInPrepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInPrepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInReset(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInReset", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInStart(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInStart", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInStop(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInStop", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveInUnprepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveInUnprepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutBreakLoop(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutBreakLoop", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutClose(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutClose", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetDevCapsA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetDevCapsA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetDevCapsW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetDevCapsW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetErrorTextA(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetErrorTextA", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetErrorTextW(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetErrorTextW", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetID(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetID", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetNumDevs(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetNumDevs", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetPitch(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetPitch", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetPlaybackRate(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetPlaybackRate", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetPosition(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetPosition", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutGetVolume(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutGetVolume", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutMessage(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutMessage", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutOpen(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutOpen", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutPause(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutPause", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutPrepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutPrepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutReset(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutReset", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutRestart(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutRestart", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutSetPitch(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutSetPitch", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutSetPlaybackRate(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutSetPlaybackRate", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutSetVolume(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutSetVolume", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutUnprepareHeader(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutUnprepareHeader", a1, a2, a3, a4, a5, a6, a7, a8);
}

UINT_PTR WINAPI proxy_waveOutWrite(UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4, UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8) {
    return forward_system_proc("waveOutWrite", a1, a2, a3, a4, a5, a6, a7, a8);
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
