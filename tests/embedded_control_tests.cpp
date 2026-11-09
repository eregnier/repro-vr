// The live-reload path reads the same keys the layer reads at start-up. A key
// read from the wrong section, or a value read with the wrong spelling, is a
// silent no-op at run time: the setting appears editable and does nothing. These
// checks pin the mapping down without a session, a GPU or a game.
#include "xrfg/embedded_control.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void write_ini(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "reprovr_live_reload";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    const auto ini = root / "repro_vr.ini";

    // Absent file: the defaults the layer starts with, not zeroes.
    {
        std::filesystem::remove(ini, ec);
        const auto settings = xrfg::embedded::read_settings_file(ini);
        require(settings.enabled, "missing file keeps generation enabled");
        require(settings.backend == 0, "missing file keeps the FidelityFX backend");
        require(settings.preset == 1, "missing file keeps the medium preset");
        require(settings.scale == 2, "missing file keeps the half input scale");
        require(settings.motion_vectors == 0, "missing file keeps estimated motion");
    }

    // The legacy section, which is where the settings tool writes today.
    {
        write_ini(ini, "[ofxr]\nenabled=0\nbackend=nvidia\nnvidia_preset=fast\n"
                       "nvidia_input_scale=100\nnvidia_bidirectional=1\n"
                       "motion_vectors=dlss\n");
        const auto settings = xrfg::embedded::read_settings_file(ini);
        require(!settings.enabled, "[ofxr] enabled=0 is read");
        require(settings.backend == 1, "[ofxr] backend=nvidia is read");
        require(settings.preset == 2, "[ofxr] nvidia_preset=fast is read");
        require(settings.scale == 0, "[ofxr] nvidia_input_scale=100 is read");
        require(settings.backward, "[ofxr] nvidia_bidirectional=1 is read");
        require(settings.motion_vectors == 1, "[ofxr] motion_vectors=dlss is read");
    }

    // The new section wins over the legacy one, which is what the layer's own
    // start-up readers do.
    {
        write_ini(ini, "[repro_vr]\nenabled=1\nnvidia_input_scale=75\n"
                       "[ofxr]\nenabled=0\nnvidia_input_scale=50\n");
        const auto settings = xrfg::embedded::read_settings_file(ini);
        require(settings.enabled, "[repro_vr] wins over [ofxr] for enabled");
        require(settings.scale == 1, "[repro_vr] wins over [ofxr] for the input scale");
    }

    // An unreadable value falls back instead of becoming zero, which would
    // otherwise switch the backend or collapse the preset.
    {
        write_ini(ini, "[ofxr]\nbackend=banana\nnvidia_preset=banana\n"
                       "nvidia_input_scale=banana\n");
        const auto settings = xrfg::embedded::read_settings_file(ini);
        require(settings.backend == 0, "unknown backend falls back to FidelityFX");
        require(settings.preset == 1, "unknown preset falls back to medium");
        require(settings.scale == 2, "unknown input scale falls back to half");
    }

    std::filesystem::remove_all(root, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d live-reload check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("live-reload settings reading checks passed\n");
    return EXIT_SUCCESS;
}
