#include "xrfg/proxy_bootstrap.hpp"

#include <windows.h>
#include <dxgi.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void write_dummy_file(const std::filesystem::path& path, const char* content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file_string(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "repro_vr_version_proxy_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);

    const std::filesystem::path source_bundle = temp_root / "bundle";
    const std::filesystem::path target_game = temp_root / "game_bin_win64";
    std::filesystem::create_directories(source_bundle);
    std::filesystem::create_directories(target_game);

    // Populate synthetic source bundle:
    if (argc > 1 && std::filesystem::exists(argv[1])) {
        std::filesystem::copy_file(
            argv[1],
            source_bundle / "version.dll",
            std::filesystem::copy_options::overwrite_existing);
    } else {
        write_dummy_file(source_bundle / "version.dll", "MZ_DUMMY_PROXY");
    }
    write_dummy_file(
        source_bundle / "XR_APILAYER_XRFrameBridge_diagnostic.dll",
        "MZ_DUMMY_LAYER");
    write_dummy_file(
        source_bundle / "ofxr_bridge.ini",
        "enabled=1\nbackend=nvidia\n");

    // -------------------------------------------------------------
    // Test 1: Fresh clean install -> selects version.dll (default)
    // -------------------------------------------------------------
    std::cout << "[TEST] 1. Fresh install into clean directory...\n";
    const auto install_res = xrfg::install_dropin_to_game_directory(
        source_bundle,
        target_game);
    require(install_res.succeeded, "install_dropin_to_game_directory must succeed");
    require(
        install_res.installed_proxy_dll.filename() == L"version.dll",
        "Default candidate for clean game must be version.dll");
    require(
        std::filesystem::exists(install_res.installed_proxy_dll),
        "Installed version.dll must exist in game directory");
    require(
        std::filesystem::exists(install_res.installed_layer_dll),
        "Installed layer DLL must exist in game directory");
    require(
        std::filesystem::exists(install_res.installed_manifest_json),
        "Installed OpenXR manifest JSON must exist in game directory");
    require(
        std::filesystem::exists(target_game / "repro_vr.ini"),
        "Installed repro_vr.ini must exist in game directory");

    // Test 2: bootstrap environment
    SetEnvironmentVariableW(L"XR_API_LAYER_PATH", nullptr);
    SetEnvironmentVariableW(L"XR_ENABLE_API_LAYERS", nullptr);
    const auto bootstrap_res = xrfg::bootstrap_repro_vr_from_directory(target_game);
    require(bootstrap_res.layer_dll_found, "bootstrap must locate the layer DLL");
    require(bootstrap_res.manifest_ready, "bootstrap must verify/write manifest JSON");
    require(bootstrap_res.environment_configured, "bootstrap must configure OpenXR env vars");

    wchar_t layer_path_env[4096]{};
    require(
        GetEnvironmentVariableW(L"XR_API_LAYER_PATH", layer_path_env, 4096) > 0,
        "XR_API_LAYER_PATH must be set in process environment");
    wchar_t enable_layers_env[1024]{};
    require(
        GetEnvironmentVariableW(L"XR_ENABLE_API_LAYERS", enable_layers_env, 1024) > 0,
        "XR_ENABLE_API_LAYERS must be set in process environment");

    // Test 2b: per-game only. Out of %TEMP% (temp manifests are never registered), the
    // bootstrap must register Explicit only, and remove an Implicit entry left by an
    // older build. Touches only this test's own value names, cleaned up below.
    {
        const auto reg_game =
            std::filesystem::absolute("repro_vr_proxy_registry_test") / "Win64";
        std::filesystem::remove_all(reg_game.parent_path(), ec);
        std::filesystem::create_directories(reg_game);
        std::filesystem::copy_file(
            target_game / "XR_APILAYER_XRFrameBridge_diagnostic.dll",
            reg_game / "XR_APILAYER_XRFrameBridge_diagnostic.dll",
            std::filesystem::copy_options::overwrite_existing);
        const std::wstring manifest =
            std::filesystem::absolute(reg_game / "XR_APILAYER_XRFrameBridge_diagnostic.json")
                .lexically_normal()
                .wstring();
        const wchar_t* implicit_key = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
        const wchar_t* explicit_key = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Explicit";
        auto has_value = [&](const wchar_t* subkey) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                              &k) != ERROR_SUCCESS) {
                return false;
            }
            const bool found = RegQueryValueExW(k, manifest.c_str(), nullptr, nullptr, nullptr,
                                                nullptr) == ERROR_SUCCESS;
            RegCloseKey(k);
            return found;
        };
        auto set_value = [&](const wchar_t* subkey) {
            HKEY k = nullptr;
            RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
            const DWORD zero = 0;
            RegSetValueExW(k, manifest.c_str(), 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
            RegCloseKey(k);
        };
        auto delete_value = [&](const wchar_t* subkey) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                              &k) == ERROR_SUCCESS) {
                RegDeleteValueW(k, manifest.c_str());
                RegCloseKey(k);
            }
        };

        set_value(implicit_key);  // what an older build left behind
        require(has_value(implicit_key), "fixture: legacy Implicit entry must exist");
        const auto reg_boot = xrfg::bootstrap_repro_vr_from_directory(reg_game);
        require(reg_boot.manifest_ready, "bootstrap must write the manifest (registry test)");
        require(!has_value(implicit_key),
                "proxy must NEVER leave a global Implicit entry (per-game only)");
        require(has_value(explicit_key),
                "proxy must register Explicit (elevated loaders ignore XR_API_LAYER_PATH)");

        delete_value(explicit_key);
        delete_value(implicit_key);
        std::filesystem::remove_all(reg_game.parent_path(), ec);
        std::cout << "[PASS] proxy registers Explicit only and cleans legacy Implicit\n";
    }

    // Uninstall Test 1:
    require(
        xrfg::remove_dropin_from_game_directory(target_game),
        "remove_dropin_from_game_directory must succeed for clean install");
    require(
        !std::filesystem::exists(install_res.installed_proxy_dll),
        "version.dll must be removed after uninstall");

    // -------------------------------------------------------------
    // Test 3: Foreign version.dll present (e.g. UEVR / ReShade)
    // -> Must pick winmm.dll and leave foreign version.dll untouched!
    // -------------------------------------------------------------
    std::cout << "[TEST] 2. Install when third-party version.dll already exists...\n";
    const std::filesystem::path foreign_ver = target_game / "version.dll";
    const std::string foreign_ver_content = "FOREIGN_UEVR_VERSION_DLL";
    write_dummy_file(foreign_ver, foreign_ver_content.c_str());

    const auto install_uevr_res = xrfg::install_dropin_to_game_directory(
        source_bundle,
        target_game);
    require(install_uevr_res.succeeded, "install must succeed when foreign version.dll is present");
    require(
        install_uevr_res.installed_proxy_dll.filename() == L"winmm.dll",
        "Proxy fallback must choose winmm.dll when version.dll exists");
    require(
        std::filesystem::exists(target_game / "winmm.dll"),
        "ReproVR must install as winmm.dll");
    require(
        read_file_string(foreign_ver) == foreign_ver_content,
        "Foreign version.dll must NOT be overwritten or corrupted!");

    // Test 4: Uninstalling ReproVR must remove winmm.dll but preserve foreign version.dll
    require(
        xrfg::remove_dropin_from_game_directory(target_game),
        "remove_dropin must succeed on fallback installation");
    require(
        !std::filesystem::exists(target_game / "winmm.dll"),
        "Installed winmm.dll must be removed on uninstall");
    require(
        std::filesystem::exists(foreign_ver) && read_file_string(foreign_ver) == foreign_ver_content,
        "Foreign version.dll must remain intact after ReproVR uninstall!");

    // -------------------------------------------------------------
    // Test 5: Foreign version.dll AND winmm.dll both present
    // -> Must pick dxgi.dll and preserve both foreign DLLs!
    // -------------------------------------------------------------
    std::cout << "[TEST] 3. Install when version.dll AND winmm.dll already exist...\n";
    const std::filesystem::path foreign_winmm = target_game / "winmm.dll";
    const std::string foreign_winmm_content = "FOREIGN_WINMM_DLL";
    write_dummy_file(foreign_winmm, foreign_winmm_content.c_str());

    const auto install_dxgi_res = xrfg::install_dropin_to_game_directory(
        source_bundle,
        target_game);
    require(install_dxgi_res.succeeded, "install must succeed when version.dll and winmm.dll are taken");
    require(
        install_dxgi_res.installed_proxy_dll.filename() == L"dxgi.dll",
        "Proxy fallback must choose dxgi.dll when version.dll and winmm.dll exist");
    require(
        std::filesystem::exists(target_game / "dxgi.dll"),
        "ReproVR must install as dxgi.dll");
    require(
        read_file_string(foreign_ver) == foreign_ver_content,
        "Foreign version.dll must remain intact!");
    require(
        read_file_string(foreign_winmm) == foreign_winmm_content,
        "Foreign winmm.dll must remain intact!");

    require(
        xrfg::remove_dropin_from_game_directory(target_game),
        "remove_dropin must succeed for dxgi install");
    require(
        !std::filesystem::exists(target_game / "dxgi.dll"),
        "Installed dxgi.dll must be removed on uninstall");
    require(
        read_file_string(foreign_ver) == foreign_ver_content,
        "Foreign version.dll must still exist!");
    require(
        read_file_string(foreign_winmm) == foreign_winmm_content,
        "Foreign winmm.dll must still exist!");

    // -------------------------------------------------------------
    // Test 6: Live runtime loading and function forwarding tests
    // -------------------------------------------------------------
    if (argc > 1 && std::filesystem::exists(argv[1])) {
        std::cout << "[TEST] 4. Live proxy forwarding tests...\n";
        const auto live_dir = temp_root / "live_test";
        std::filesystem::create_directories(live_dir);

        // A. Load as version.dll -> test GetFileVersionInfoSizeW
        const auto live_version = live_dir / "version.dll";
        std::filesystem::copy_file(argv[1], live_version, std::filesystem::copy_options::overwrite_existing);

        HMODULE mod_ver = LoadLibraryW(live_version.c_str());
        require(mod_ver != nullptr, "Loading built proxy as version.dll must succeed");

        using PfnGetFileVersionInfoSizeW = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
        auto fn_ver_size = reinterpret_cast<PfnGetFileVersionInfoSizeW>(
            GetProcAddress(mod_ver, "GetFileVersionInfoSizeW"));
        require(fn_ver_size != nullptr, "version.dll proxy must export GetFileVersionInfoSizeW");

        wchar_t sys_dir[MAX_PATH]{};
        GetSystemDirectoryW(sys_dir, MAX_PATH);
        const std::filesystem::path kernel32_path =
            std::filesystem::path(sys_dir) / L"kernel32.dll";
        DWORD dummy_handle = 0;
        const DWORD ver_size = fn_ver_size(kernel32_path.c_str(), &dummy_handle);
        require(ver_size > 0, "Forwarded GetFileVersionInfoSizeW must return > 0 bytes");
        FreeLibrary(mod_ver);

        // B. Load as winmm.dll -> test timeGetTime
        const auto live_winmm = live_dir / "winmm.dll";
        std::filesystem::copy_file(argv[1], live_winmm, std::filesystem::copy_options::overwrite_existing);

        HMODULE mod_winmm = LoadLibraryW(live_winmm.c_str());
        require(mod_winmm != nullptr, "Loading built proxy as winmm.dll must succeed");

        using PfnTimeGetTime = DWORD(WINAPI*)();
        auto fn_time = reinterpret_cast<PfnTimeGetTime>(
            GetProcAddress(mod_winmm, "timeGetTime"));
        require(fn_time != nullptr, "winmm.dll proxy must export timeGetTime");

        const DWORD t1 = fn_time();
        require(t1 > 0, "Forwarded timeGetTime must return > 0 milliseconds");
        FreeLibrary(mod_winmm);

        // C. Load as dxgi.dll -> test CreateDXGIFactory1
        const auto live_dxgi = live_dir / "dxgi.dll";
        std::filesystem::copy_file(argv[1], live_dxgi, std::filesystem::copy_options::overwrite_existing);

        HMODULE mod_dxgi = LoadLibraryW(live_dxgi.c_str());
        require(mod_dxgi != nullptr, "Loading built proxy as dxgi.dll must succeed");

        using PfnCreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
        auto fn_dxgi = reinterpret_cast<PfnCreateDXGIFactory1>(
            GetProcAddress(mod_dxgi, "CreateDXGIFactory1"));
        require(fn_dxgi != nullptr, "dxgi.dll proxy must export CreateDXGIFactory1");

        IDXGIFactory1* pFactory = nullptr;
        const HRESULT hr = fn_dxgi(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory));
        require(SUCCEEDED(hr) && pFactory != nullptr, "Forwarded CreateDXGIFactory1 must succeed");
        pFactory->Release();
        FreeLibrary(mod_dxgi);
    }

    std::filesystem::remove_all(temp_root, ec);
    std::cout << "PASS: xrfg_version_proxy_tests\n";
    return EXIT_SUCCESS;
}
