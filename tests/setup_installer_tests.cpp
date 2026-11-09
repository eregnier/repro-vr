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

int main() {
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

    Require(!installed(dummyTarget), "Target must not be reported as installed before installation");

    // 3. Test install()
    std::wstring err;
    bool installOk = install(dummyTarget, dummySource, &err);
    Require(installOk, "install() must succeed in sandbox");
    Require(installed(dummyTarget), "installed() must report true after install");

    // Check manifest JSON
    fs::path manifestPath = dummyTarget / kLayerManifest;
    Require(fs::exists(manifestPath), "Manifest JSON must exist in target game folder");
    {
        std::ifstream mf(manifestPath);
        std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        Require(content.find("XR_APILAYER_XRFrameBridge_diagnostic") != std::string::npos,
                "Manifest must contain layer name");
        Require(content.find("api_layer") != std::string::npos,
                "Manifest must contain api_layer definition");
    }

    // Check INI existence
    Require(fs::exists(dummyTarget / L"repro_vr.ini"), "repro_vr.ini must exist in target folder");
    std::cout << "[PASS] install() verified with DLL, manifest JSON, and repro_vr.ini" << std::endl;

    // 4. Test uninstall()
    bool uninstallOk = uninstall(dummyTarget, &err);
    Require(uninstallOk, "uninstall() must succeed");
    Require(!installed(dummyTarget), "Target must not be reported as installed after uninstall");
    Require(!fs::exists(dummyTarget / kLayerDll), "DLL must be removed after uninstall");
    Require(!fs::exists(manifestPath), "Manifest JSON must be removed after uninstall");
    Require(fs::exists(dummyTarget / L"repro_vr.ini"), "repro_vr.ini must be preserved after uninstall");
    std::cout << "[PASS] uninstall() verified: layer files cleaned, config preserved" << std::endl;

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
