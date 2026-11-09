#include "xrfg/proxy_bootstrap.hpp"

#include <windows.h>

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

    // 1. Test one-click drop-in installation to game directory:
    const auto install_res = xrfg::install_dropin_to_game_directory(
        source_bundle,
        target_game);
    require(install_res.succeeded, "install_dropin_to_game_directory must succeed");
    require(
        std::filesystem::exists(install_res.installed_proxy_dll),
        "Installed version.dll must exist in game directory");
    require(
        std::filesystem::exists(install_res.installed_layer_dll),
        "Installed layer DLL must exist in game directory");
    require(
        std::filesystem::exists(install_res.installed_manifest_json),
        "Installed OpenXR manifest JSON must exist in game directory");
    // The canonical name since the configuration was unified into repro_vr.ini;
    // the legacy ofxr_bridge.ini is only accepted as an installation source.
    require(
        std::filesystem::exists(target_game / "repro_vr.ini"),
        "Installed repro_vr.ini must exist in game directory");

    // 2. Test bootstrap_repro_vr_from_directory on the installed game directory:
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
    require(
         std::wstring(enable_layers_env).find(L"XR_APILAYER_XRFrameBridge_diagnostic") !=
            std::wstring::npos,
        "XR_ENABLE_API_LAYERS must include XR_APILAYER_XRFrameBridge_diagnostic");

    // 3. If the built version.dll proxy path was passed on command line, load it
    // and verify live forwarding of GetFileVersionInfoSizeW to System32\version.dll:
    if (argc > 1 && std::filesystem::exists(argv[1])) {
        HMODULE proxy_mod = LoadLibraryW(install_res.installed_proxy_dll.c_str());
        require(proxy_mod != nullptr, "Loading built version.dll proxy must succeed");

        using PfnGetFileVersionInfoSizeW = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
        auto fn_size = reinterpret_cast<PfnGetFileVersionInfoSizeW>(
            GetProcAddress(proxy_mod, "GetFileVersionInfoSizeW"));
        require(
            fn_size != nullptr,
            "version.dll proxy must export GetFileVersionInfoSizeW");

        wchar_t system_dir[MAX_PATH]{};
        GetSystemDirectoryW(system_dir, MAX_PATH);
        const std::filesystem::path kernel32_path =
            std::filesystem::path(system_dir) / L"kernel32.dll";
        DWORD dummy_handle = 0;
        const DWORD ver_size = fn_size(kernel32_path.c_str(), &dummy_handle);
        require(
            ver_size > 0,
            "Forwarded GetFileVersionInfoSizeW(kernel32.dll) must return > 0 bytes");

        FreeLibrary(proxy_mod);
    }

    // 4. Test frictionless one-click removal from game directory:
    require(
        xrfg::remove_dropin_from_game_directory(target_game),
        "remove_dropin_from_game_directory must succeed");
    require(
        !std::filesystem::exists(install_res.installed_proxy_dll),
        "version.dll must be removed after uninstall");
    require(
        !std::filesystem::exists(install_res.installed_layer_dll),
        "Layer DLL must be removed after uninstall");
    require(
        !std::filesystem::exists(install_res.installed_manifest_json),
        "Manifest JSON must be removed after uninstall");

    std::filesystem::remove_all(temp_root, ec);
    std::cout << "PASS: xrfg_version_proxy_tests\n";
    return EXIT_SUCCESS;
}
