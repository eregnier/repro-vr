#include "xrfg/embedded_control.hpp"
#include "xrfg/implicit_layer.hpp"
#include <windows.h>
#include <map>
#include <mutex>
#include <algorithm>
#include <atomic>

namespace xrfg::embedded {
namespace {
std::filesystem::path directory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&directory), &module);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    return std::filesystem::path(path).parent_path();
}
// The layer's own resolver, so the file polled here is the file it read at
// start-up: repro_vr.ini when present, the legacy ofxr_bridge.ini otherwise.
const std::filesystem::path& ini() {
    static auto path = implicit_layer::resolve_config_ini_path(directory());
    return path;
}
struct Receipt { std::uint64_t revision{}; bool enabled{}; long error{}; };
struct Control {
    std::mutex mutex;
    Settings settings;
    std::uint64_t revision{1}, next_id{1};
    std::map<std::uint64_t, Receipt> sessions;
    Control() {
        const auto options = implicit_layer::read_nvidia_options(directory());
        settings.backend = static_cast<int>(implicit_layer::read_flow_backend(directory()));
        settings.preset = static_cast<int>(options.preset);
        settings.scale = static_cast<int>(options.input_scale);
        settings.backward = options.bidirectional;
        settings.enabled = GetPrivateProfileIntW(L"ofxr", L"enabled", 1, ini().c_str()) != 0;
        wchar_t motion[16]{};
        GetPrivateProfileStringW(L"ofxr", L"motion_vectors", L"off", motion, 16, ini().c_str());
        settings.motion_vectors = _wcsicmp(motion, L"dlss") == 0 ? 1 : 0;
    }
};
Control& control() { static Control state; return state; }
bool write(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return WritePrivateProfileStringW(section, key, value, ini().c_str()) != FALSE;
}
constexpr const wchar_t* positions[]{L"off", L"upper_left", L"upper_right", L"lower_left", L"lower_right"};
}
Snapshot snapshot() {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    Snapshot s; s.desired = c.settings; s.revision = c.revision;
    s.sessions = static_cast<unsigned>(c.sessions.size());
    for (auto& [id, r] : c.sessions) {
        if (r.revision != c.revision) ++s.pending;
        else if (r.error) { ++s.errors; s.last_error = r.error; }
        else if (r.enabled) ++s.ready;
        else ++s.bypass;
    }
    return s;
}
bool request(Settings s) {
    if (s.backend < 0 || s.backend > 1 || s.preset < 0 || s.preset > 2 ||
        s.scale < 0 || s.scale > 2 || s.motion_vectors < 0 || s.motion_vectors > 1 ||
        s.frame_generation != 0) return false;
    auto& c = control(); std::scoped_lock lock(c.mutex);
    const auto old = c.settings;
    bool ok = true;
    // Do not rewrite diagnostics, comments, or unrelated configuration.
    if (old.enabled != s.enabled) ok &= write(L"ofxr", L"enabled", s.enabled ? L"1" : L"0");
    if (old.backend != s.backend) ok &= write(L"ofxr", L"backend", s.backend ? L"nvidia" : L"fidelityfx");
    const wchar_t* presets[]{L"slow", L"medium", L"fast"};
    const wchar_t* scales[]{L"100", L"75", L"50"};
    if (old.preset != s.preset) ok &= write(L"ofxr", L"nvidia_preset", presets[s.preset]);
    if (old.scale != s.scale) ok &= write(L"ofxr", L"nvidia_input_scale", scales[s.scale]);
    if (old.backward != s.backward) ok &= write(L"ofxr", L"nvidia_bidirectional", s.backward ? L"1" : L"0");
    if (old.motion_vectors != s.motion_vectors) ok &= write(L"ofxr", L"motion_vectors", s.motion_vectors ? L"dlss" : L"off");
    if (old != s) { c.settings = s; ++c.revision; }
    return ok;
}
std::uint64_t attach() { auto& c = control(); std::scoped_lock lock(c.mutex); auto id = c.next_id++; c.sessions.emplace(id, Receipt{}); return id; }
void detach(std::uint64_t id) { auto& c = control(); std::scoped_lock lock(c.mutex); c.sessions.erase(id); }
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error) {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    if (auto it = c.sessions.find(id); it != c.sessions.end()) it->second = {revision, enabled, error};
}
namespace {
// Same three sections, same order, as implicit_layer's compat readers: a value
// written under the new section wins, and the legacy one still works.
std::wstring control_string(const wchar_t* key, const wchar_t* fallback,
                            const std::filesystem::path& ini_path) {
    static const wchar_t kMissing[] = L"__MISSING__";
    wchar_t buffer[64]{};
    for (const wchar_t* section : {L"repro_vr", L"reprovr", L"ofxr"}) {
        GetPrivateProfileStringW(section, key, kMissing, buffer, 64, ini_path.c_str());
        if (_wcsicmp(buffer, kMissing) != 0) return buffer;
    }
    return fallback;
}
int control_int(const wchar_t* key, int fallback, const std::filesystem::path& ini_path) {
    // Empty, never a null pointer: std::wstring cannot be built from nullptr.
    const auto text = control_string(key, L"", ini_path);
    if (text.empty()) return fallback;
    return _wtoi(text.c_str());
}
} // namespace

Settings read_settings_file(const std::filesystem::path& ini_path) {
    Settings s{};
    if (ini_path.empty()) return s;
    s.enabled = control_int(L"enabled", 1, ini_path) != 0;
    s.backend = _wcsicmp(control_string(L"backend", L"fidelityfx", ini_path).c_str(),
                         L"nvidia") == 0 ? 1 : 0;
    const auto preset = control_string(L"nvidia_preset", L"medium", ini_path);
    s.preset = _wcsicmp(preset.c_str(), L"slow") == 0 ? 0
             : _wcsicmp(preset.c_str(), L"fast") == 0 ? 2 : 1;
    s.scale = control_int(L"nvidia_input_scale", 50, ini_path) == 100 ? 0
            : control_int(L"nvidia_input_scale", 50, ini_path) == 75 ? 1 : 2;
    s.backward = control_int(L"nvidia_bidirectional", 0, ini_path) != 0;
    s.motion_vectors = _wcsicmp(control_string(L"motion_vectors", L"off", ini_path).c_str(),
                                L"dlss") == 0 ? 1 : 0;
    return s;
}

bool reload_from_ini() {
    Settings wanted{};
    try {
        wanted = read_settings_file(ini());
    } catch (...) {
        return false;
    }
    // request() returns false when the INI write fails, but it still applies the
    // value in memory; only a real change is reported here.
    const bool changed = !(wanted == snapshot().desired);
    if (changed) request(wanted);
    return changed;
}

void poll_ini() {
    static std::atomic<std::uint64_t> next_check{};
    const std::uint64_t now = GetTickCount64();
    auto due = next_check.load(std::memory_order_relaxed);
    if (now < due) return;
    if (!next_check.compare_exchange_strong(due, now + 500,
                                            std::memory_order_relaxed)) return;
    reload_from_ini();
}

bool logging_setting() { return GetPrivateProfileIntW(L"diagnostics", L"logging_enabled", 0, ini().c_str()) != 0; }
bool set_logging_setting(bool enabled) { return write(L"diagnostics", L"logging_enabled", enabled ? L"1" : L"0"); }
int overlay_setting() {
    wchar_t value[32]{}; GetPrivateProfileStringW(L"overlay", L"position", L"upper_right", value, 32, ini().c_str());
    for (int i = 0; i < 5; ++i) if (_wcsicmp(value, positions[i]) == 0) return i;
    return 2;
}
bool set_overlay_setting(int position) { return position >= 0 && position < 5 && write(L"overlay", L"position", positions[position]); }
}

// Private, versioned POD ABI for the combined-module host regression harness.
extern "C" __declspec(dllexport) int OFXR_EmbeddedRequestV1(int enabled, int backend, int preset, int scale, int backward) noexcept {
    try { return xrfg::embedded::request({enabled != 0, backend, preset, scale, backward != 0, 0, 0}) ? 1 : 0; }
    catch (...) { return 0; }
}
