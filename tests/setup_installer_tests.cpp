#include <windows.h>
#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include "installer.hpp"

namespace fs = std::filesystem;
using namespace reprovr::setup;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED REQUIREMENT: " << message << std::endl;
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    std::cout << "[TEST] Starting ReproVR Setup & Installer tests..." << std::endl;

    // 1. Test store path resolution
    auto store = store_path();
    Require(!store.empty(), "store_path() must return a non-empty path");
    std::cout << "[PASS] store_path: " << store.string() << std::endl;

    // 2. Setup temporary sandbox for installation testing
    wchar_t tempPathBuf[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPathBuf);
    fs::path tempRoot = fs::path(tempPathBuf) / L"ReproVR_Installer_Test_Sandbox";
    std::error_code ec;
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);

    fs::path dummySource = tempRoot / L"source";
    fs::path dummyTarget = tempRoot / L"game";
    fs::create_directories(dummySource, ec);
    fs::create_directories(dummyTarget, ec);

    // Create mock layer dll in source
    fs::path mockDll = dummySource / kLayerDll;
    {
        std::ofstream out(mockDll, std::ios::binary);
        out << "MOCK_REPROVR_LAYER_DLL_BYTES";
    }

    fs::path manifestPath = dummyTarget / kLayerManifest;
    Require(!installed(dummyTarget), "Target must not be reported as installed before installation");
    Require(!is_implicit_layer_registered(manifestPath), "Manifest must not be registered before install");

    // 3. Test install()
    std::wstring err;
    bool installOk = install(dummyTarget, dummySource, &err);
    Require(installOk, "install() must succeed in sandbox");
    Require(installed(dummyTarget), "installed() must report true after install");

    // Check manifest JSON
    Require(fs::exists(manifestPath), "Manifest JSON must exist in target game folder");
    {
        std::ifstream mf(manifestPath);
        std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        Require(content.find("XR_APILAYER_XRFrameBridge_diagnostic") != std::string::npos,
                "Manifest must contain layer name");
        Require(content.find("api_layer") != std::string::npos,
                "Manifest must contain api_layer definition");
    }

    // Check OpenXR Implicit Layer registry registration
    Require(!is_implicit_layer_registered(manifestPath),
            "Manifest must NOT be registered globally in OpenXR Implicit ApiLayers (per-game only)");

    // Check INI existence
    Require(fs::exists(dummyTarget / L"repro_vr.ini"), "repro_vr.ini must exist in target folder");
    std::cout << "[PASS] install() verified with DLL, manifest JSON, registry key and repro_vr.ini" << std::endl;

    // 4. Test uninstall()
    bool uninstallOk = uninstall(dummyTarget, &err);
    Require(uninstallOk, "uninstall() must succeed");
    Require(!installed(dummyTarget), "Target must not be reported as installed after uninstall");
    Require(!fs::exists(dummyTarget / kLayerDll), "DLL must be removed after uninstall");
    Require(!fs::exists(manifestPath), "Manifest JSON must be removed after uninstall");
    Require(!is_implicit_layer_registered(manifestPath),
            "Manifest must be removed from OpenXR Implicit ApiLayers registry after uninstall");
    Require(fs::exists(dummyTarget / L"repro_vr.ini"), "repro_vr.ini must be preserved after uninstall");
    std::cout << "[PASS] uninstall() verified: layer files cleaned, registry cleaned, config preserved" << std::endl;

    // 5. Test idempotency of registry registration helpers
    Require(unregister_implicit_layer(manifestPath), "Unregistering again must be safe and idempotent");
    Require(!is_implicit_layer_registered(manifestPath), "Must remain unregistered");
    Require(register_implicit_layer(manifestPath), "Standalone register_implicit_layer must succeed");
    Require(is_implicit_layer_registered(manifestPath), "Standalone registration must be detected");
    Require(unregister_implicit_layer(manifestPath), "Standalone cleanup must succeed");
    Require(!is_implicit_layer_registered(manifestPath), "Cleaned up manifest must not be registered");
    std::cout << "[PASS] OpenXR registry helper idempotency and lifecycle verified" << std::endl;

    // 5b. Legacy migration, on a SCRATCH registry key: the real OpenXR key is never touched.
    {
        Require(argc >= 5, "fixture paths (built by CMake) must be passed to this test");
        const fs::path fx_version_exe = argv[3];
        const fs::path real_proxy = argv[4];
        const std::wstring scratch = L"SOFTWARE\\ReproVRTest\\Implicit";
        const fs::path root = fs::temp_directory_path() / L"reprovr_legacy_test";
        fs::remove_all(root);
        fs::create_directories(root);
        auto put = [&](const fs::path& manifest, const char* json) {
            fs::create_directories(manifest.parent_path());
            std::ofstream(manifest) << json;
            HKEY k = nullptr;
            RegCreateKeyExW(HKEY_CURRENT_USER, scratch.c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
            const DWORD zero = 0;
            RegSetValueExW(k, manifest.c_str(), 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
            RegCloseKey(k);
        };
        auto present = [&](const fs::path& manifest) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, scratch.c_str(), 0,
                              KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
            const bool ok = RegQueryValueExW(k, manifest.c_str(), nullptr, nullptr, nullptr,
                                             nullptr) == ERROR_SUCCESS;
            RegCloseKey(k);
            return ok;
        };
        LegacyOptions opt;
        opt.implicit_subkey = scratch;
        opt.include_machine = false;
        opt.backup_file = root / L"backup.ini";
        opt.log_file = root / L"migration.log";

        // Known game (installer's own manifest name), unknown game, foreign layer in a
        // folder called "reprovr".
        const fs::path known = root / L"known" / kLayerManifest;
        const fs::path unknown = root / L"unknown" / L"XR_APILAYER_reprovr.json";
        const fs::path nodll = root / L"nodll" / L"XR_APILAYER_reprovr.json";
        const fs::path foreign = root / L"reprovr" / L"XR_APILAYER_SOMEONEELSE.json";
        put(known, "{\"api_layer\":{\"name\":\"XR_APILAYER_XRFrameBridge_diagnostic\"}}");
        put(unknown, "{\"api_layer\":{\"name\":\"XR_APILAYER_REPROVR_frame_synthesis\"}}");
        put(nodll, "{\"api_layer\":{\"name\":\"XR_APILAYER_REPROVR_frame_synthesis\"}}");
        std::ofstream(unknown.parent_path() / L"XR_APILAYER_reprovr.dll") << "dll";
        put(foreign, "{\"api_layer\":{\"name\":\"XR_APILAYER_SOMEONEELSE\"}}");

        // Detection: ours yes, foreign never.
        int seen = 0;
        for (const auto& e : find_legacy_global_layers(opt)) {
            Require(e.manifest != foreign, "foreign layer must never be detected as ReproVR");
            ++seen;
        }
        Require(seen == 3, "all three ReproVR entries must be detected");

        // Dry run: reports, changes nothing.
        LegacyOptions dry = opt;
        dry.dry_run = true;
        const auto dryReport = repair_legacy_global_layers(dummySource, dry);
        Require(dryReport.found == 3 && dryReport.removed == 0, "dry-run must not remove anything");
        Require(present(known) && present(unknown) && present(nodll), "dry-run must leave the registry intact");
        Require(!fs::exists(opt.backup_file) && !fs::exists(known.parent_path() / kLayerDll),
                "dry-run must not write backup nor install");

        // Safety: re-arm impossible (source without DLL) => the known game's entry is KEPT.
        const fs::path emptySource = root / L"empty_source";
        fs::create_directories(emptySource);
        const auto kept = repair_legacy_global_layers(emptySource, opt);
        Require(present(known) && present(unknown), "entries must be KEPT when the per-game install fails");
        Require(kept.kept.size() == 2, "failed re-arm must be reported as kept");
        Require(!present(nodll) && !kept.not_rearmed.empty(),
                "folder without a ReproVR DLL: removed and reported, never re-armed");

        // Safety: re-armed, but nothing in the folder shows the game loads the proxy
        // (no game exe) => the global entry is KEPT so the game keeps working.
        const fs::path legacySource = root / L"legacy_source";
        fs::create_directories(legacySource);
        fs::copy_file(dummySource / kLayerDll, legacySource / kLayerDll);
        fs::copy_file(real_proxy, legacySource / L"version.dll");
        const auto unverified = repair_legacy_global_layers(legacySource, opt);
        Require(present(known) && present(unknown),
                "entries must be KEPT when the game may not load the per-game proxy");
        Require(unverified.unverified.size() == 2 && unverified.kept.size() == 2,
                "unverified games must be reported");

        // The game folders now hold a game exe importing version.dll.
        fs::copy_file(fx_version_exe, known.parent_path() / L"Game.exe");
        fs::copy_file(fx_version_exe, unknown.parent_path() / L"Game.exe");

        // Real run: known game re-armed per game and entry removed, foreign untouched.
        const auto report = repair_legacy_global_layers(legacySource, opt);
        Require(!present(known) && !present(unknown), "legacy entries must be removed");
        Require(fs::exists(unknown.parent_path() / kLayerDll), "unknown game WITH a ReproVR DLL must be re-armed per game");
        Require(!fs::exists(nodll.parent_path() / kLayerDll), "folder without ReproVR DLL must not receive an install");
        Require(present(foreign), "foreign layer must be left untouched");
        Require(fs::exists(known.parent_path() / kLayerDll), "known game must be re-armed per game");
        Require(report.reinstalled == 2, "known + DLL-bearing game must each be re-armed once");
        Require(!kept.not_rearmed.empty(),
                "unknown game (removed on the first pass) must be reported as not re-armed");
        Require(fs::exists(opt.backup_file), "a backup must be written before deletion");

        // Idempotent.
        Require(repair_legacy_global_layers(legacySource, opt).found == 0, "second run finds nothing");

        // Rollback.
        Require(restore_legacy_global_layers(opt) == 3, "restore must bring all entries back");
        Require(present(known) && present(unknown), "restored entries must be present");

        Require(proxy_will_load(known.parent_path()) && proxy_will_load(unknown.parent_path()),
                "re-armed games must have a proxy the game loads");
        Require(RegDeleteTreeW(HKEY_CURRENT_USER, L"SOFTWARE\\ReproVRTest") == ERROR_SUCCESS,
                "scratch registry key must be cleaned up");
        fs::remove_all(root);
        std::cout << "[PASS] legacy migration: dry-run, keep-on-failure, re-arm, backup, restore, foreign untouched" << std::endl;
    }

    // 5c. Proxy choice follows what the game really imports (measured: SkyrimVR.exe
    // imports winmm/dxgi but not version.dll). Fixtures are tiny exes built by CMake.
    if (argc >= 4) {
        const fs::path fx_winmm = argv[1], fx_delay = argv[2], fx_version = argv[3];
        const fs::path root = tempRoot / L"proxy_choice";
        const fs::path src = root / L"source";
        fs::create_directories(src);
        std::ofstream(src / kLayerDll, std::ios::binary) << "MOCK_LAYER";
        std::ofstream(src / L"version.dll", std::ios::binary) << "MOCK_PROXY";
        auto marker_proxy = [](const fs::path& dir) {
            std::ifstream mf(dir / L"repro_vr_installed.manifest");
            std::string line;
            while (std::getline(mf, line)) {
                if (line.rfind("proxy=", 0) == 0) return line.substr(6);
            }
            return std::string();
        };
        auto game_with = [&](const wchar_t* name, const fs::path& exe) {
            const fs::path dir = root / name;
            fs::create_directories(dir);
            if (!exe.empty()) fs::copy_file(exe, dir / L"Game.exe", fs::copy_options::overwrite_existing);
            return dir;
        };

        const auto g_winmm = game_with(L"winmm", fx_winmm);
        Require(install(g_winmm, src, &err), "install (winmm game)");
        Require(marker_proxy(g_winmm) == "winmm.dll", "game importing winmm.dll must get the winmm.dll proxy");
        Require(fs::exists(g_winmm / L"winmm.dll") && !fs::exists(g_winmm / L"version.dll"),
                "only the chosen proxy file is written");
        Require(proxy_will_load(g_winmm), "winmm game: proxy confirmed");

        const auto g_delay = game_with(L"delay", fx_delay);
        Require(install(g_delay, src, &err), "install (delay-load game)");
        Require(marker_proxy(g_delay) == "winmm.dll", "delay-loaded winmm.dll must be detected");
        Require(proxy_will_load(g_delay), "delay-load game: proxy confirmed");

        const auto g_version = game_with(L"version", fx_version);
        Require(install(g_version, src, &err), "install (version game)");
        Require(marker_proxy(g_version) == "version.dll", "game importing version.dll keeps version.dll");
        Require(proxy_will_load(g_version), "version game: proxy confirmed");

        const auto g_none = game_with(L"none", fs::path());
        Require(install(g_none, src, &err), "install (no evidence)");
        Require(marker_proxy(g_none) == "version.dll", "no evidence: historical default version.dll");
        Require(!proxy_will_load(g_none), "no evidence: proxy NOT confirmed (info message shown)");

        // A helper exe (crash handler) importing version.dll is not the game: no evidence.
        // Measured on PLAYA VR: only UnityCrashHandler64.exe imports version.dll there.
        const fs::path g_helper = root / L"helper";
        fs::create_directories(g_helper);
        fs::copy_file(fx_version, g_helper / L"UnityCrashHandler64.exe");
        Require(install(g_helper, src, &err), "install (helper exe only)");
        Require(!proxy_will_load(g_helper), "a crash handler's imports must not count as the game's");

        // Foreign version.dll already there (UEVR, ReShade...): never overwritten.
        const auto g_foreign = game_with(L"foreign", fx_version);
        std::ofstream(g_foreign / L"version.dll", std::ios::binary) << "FOREIGN_UEVR";
        Require(install(g_foreign, src, &err), "install (foreign version.dll)");
        {
            std::ifstream f(g_foreign / L"version.dll", std::ios::binary);
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Require(s == "FOREIGN_UEVR", "a foreign version.dll must never be overwritten");
        }
        Require(marker_proxy(g_foreign) != "version.dll", "another proxy name must be used");
        Require(!proxy_will_load(g_foreign), "foreign case: proxy not confirmed (info message shown)");

        std::cout << "[PASS] proxy choice follows the game's real imports; foreign DLLs untouched" << std::endl;
    } else {
        std::cout << "[SKIP] proxy choice: fixture paths not given" << std::endl;
    }


    // 5. Test discover_steam_games()
    auto discovered = discover_steam_games();
    std::cout << "[PASS] discover_steam_games() executed cleanly, found " << discovered.size() << " games" << std::endl;
    for (const auto& g : discovered) {
        std::wcout << L"   -> Discovered: " << g.name << L" in " << g.directory << std::endl;
    }

    // Cleanup sandbox
    fs::remove_all(tempRoot, ec);

    std::cout << "[ALL PASS] All ReproVR setup & installer tests completed successfully!" << std::endl;
    return 0;
}
