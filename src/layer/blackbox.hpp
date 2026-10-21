// ReproVR "black box": crash-surviving, near-zero-overhead run recorder.
//
//  * reprovr_phase.bin  - 64-byte memory-mapped record (last phase, frame, tick,
//                         last HRESULT). Plain stores on the hot path; a background
//                         thread flushes it every 250 ms, so after a hang / TDR / BSOD
//                         the last phase reached is readable (tools/analyze_run.py).
//  * reprovr_blackbox.log - run header (build, ini, module, run_id) + one line per
//                         second (fps, EndFrame p50/p99/max, synth counters, errors).
//                         Written by the background thread only; the render thread
//                         never touches the disk and never flushes.
//
// Enabled only when repro_vr_installed.manifest sits next to the layer DLL.
#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace xrfg::bb {

enum class Phase : std::uint32_t {
    none = 0,
    session_created,
    end_frame_begin,
    capture,
    synth_prime_submit,
    synth_pair_submit,
    synth_done,
    publish,
    runtime_end_frame,
    end_frame_end,
};

inline const char* phase_name(std::uint32_t p) noexcept {
    static constexpr const char* names[] = {
        "none", "session_created", "end_frame_begin", "capture", "synth_prime_submit",
        "synth_pair_submit", "synth_done", "publish", "runtime_end_frame", "end_frame_end"};
    return p < sizeof(names) / sizeof(names[0]) ? names[p] : "?";
}

struct PhaseRecord {            // 64 bytes, memory mapped
    std::uint32_t magic;        // 'RBB1'
    std::uint32_t run_id;
    volatile std::uint32_t phase;
    volatile std::uint32_t last_hr;
    volatile std::uint64_t frame;
    volatile std::uint64_t tick_ms;     // GetTickCount64 at last phase change
    volatile std::uint64_t synth_frames;
    volatile std::uint32_t errors;
    volatile std::uint32_t thread_id;
    volatile std::uint32_t hmd_hz_milli;
    volatile std::uint16_t multiplier;
    volatile std::uint16_t pad16;
    volatile std::uint32_t last_wait_us;
    volatile std::uint32_t last_synth_us;
};
static_assert(sizeof(PhaseRecord) == 64);

#pragma pack(push, 1)
struct FrameFlightSample {
    std::uint64_t frame;
    std::uint32_t tick_ms;
    float dt_ms;
    float atw_err_deg;
    float border_px;
    float frac;
    std::uint32_t synth_us;
    std::uint32_t wait_us;
    float q_real[4];
    float q_synth[4];
};
#pragma pack(pop)

struct State {
    std::atomic<int> enabled{0};            // 0 unknown, 1 on, -1 off
    PhaseRecord* rec{nullptr};
    HANDLE map{nullptr};
    HANDLE log{INVALID_HANDLE_VALUE};
    HANDLE flight_file{INVALID_HANDLE_VALUE};
    std::atomic<std::uint64_t> frames{0}, synth{0}, errors{0};
    static constexpr std::size_t kRing = 512;
    std::array<std::atomic<std::uint32_t>, kRing> end_frame_us{};  // microseconds
    std::atomic<std::uint32_t> ring_pos{0};
    std::atomic<std::uint32_t> hmd_hz_milli{0};
    std::atomic<std::uint32_t> current_mult{2};
    std::atomic<std::uint32_t> last_synth_us{0};
    std::atomic<std::uint32_t> last_wait_us{0};

    static constexpr std::size_t kFlightSamples = 8192;
    std::array<FrameFlightSample, kFlightSamples> flight_ring{};
    std::atomic<std::uint32_t> flight_pos{0};
    std::uint32_t flight_flushed{0};
    std::wstring layer_dir;

    std::atomic<bool> stop{false};
};

inline State& st() noexcept {
    static State s;
    return s;
}

inline void write_line(const char* text, int len) noexcept {
    auto& s = st();
    if (s.log != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(s.log, text, static_cast<DWORD>(len), &w, nullptr);
    }
}

inline void logf(const char* fmt, ...) noexcept {
    char buf[1024];
    SYSTEMTIME t{};
    GetLocalTime(&t);
    int n = std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    int b = std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, a);
    va_end(a);
    n = std::min<int>(n + (b > 0 ? b : 0), static_cast<int>(sizeof(buf)) - 2);
    buf[n++] = '\n';
    write_line(buf, n);
}

inline void set_phase(Phase p) noexcept {
    auto& s = st();
    if (s.enabled.load(std::memory_order_relaxed) <= 0 || !s.rec) return;
    s.rec->phase = static_cast<std::uint32_t>(p);
    s.rec->frame = s.frames.load(std::memory_order_relaxed);
    s.rec->tick_ms = GetTickCount64();
    s.rec->thread_id = GetCurrentThreadId();
}

inline void note_hr(long hr) noexcept {
    auto& s = st();
    if (s.enabled.load(std::memory_order_relaxed) <= 0 || !s.rec) return;
    s.rec->last_hr = static_cast<std::uint32_t>(hr);
    if (hr < 0) {
        s.errors.fetch_add(1, std::memory_order_relaxed);
        s.rec->errors = static_cast<std::uint32_t>(s.errors.load(std::memory_order_relaxed));
        logf("ERROR hr=0x%08lX phase=%s frame=%llu", static_cast<unsigned long>(hr),
             phase_name(s.rec->phase), static_cast<unsigned long long>(s.rec->frame));
    }
}

inline void note_synth() noexcept {
    auto& s = st();
    if (s.enabled.load(std::memory_order_relaxed) <= 0) return;
    const auto n = s.synth.fetch_add(1, std::memory_order_relaxed) + 1;
    if (s.rec) s.rec->synth_frames = n;
}

inline void set_hmd_refresh(float hz) noexcept {
    auto& s = st();
    const auto m = static_cast<std::uint32_t>(hz * 1000.0f);
    s.hmd_hz_milli.store(m, std::memory_order_relaxed);
    if (s.rec) s.rec->hmd_hz_milli = m;
}

inline void set_multiplier(std::uint32_t mult) noexcept {
    auto& s = st();
    s.current_mult.store(mult, std::memory_order_relaxed);
    if (s.rec) s.rec->multiplier = static_cast<std::uint16_t>(mult);
}

inline void note_synth_duration(std::uint32_t us) noexcept {
    auto& s = st();
    s.last_synth_us.store(us, std::memory_order_relaxed);
    if (s.rec) s.rec->last_synth_us = us;
}

inline void note_wait_duration(std::uint32_t us) noexcept {
    auto& s = st();
    s.last_wait_us.store(us, std::memory_order_relaxed);
    if (s.rec) s.rec->last_wait_us = us;
}

inline void record_flight_sample(
    std::uint64_t frame, float dt_ms, float atw_err, float border_px, float frac,
    std::uint32_t synth_us, std::uint32_t wait_us,
    const float q_real[4], const float q_synth[4]) noexcept {
    auto& s = st();
    if (s.enabled.load(std::memory_order_relaxed) <= 0) return;
    const std::uint32_t idx = s.flight_pos.fetch_add(1, std::memory_order_relaxed) % State::kFlightSamples;
    auto& sample = s.flight_ring[idx];
    sample.frame = frame;
    sample.tick_ms = static_cast<std::uint32_t>(GetTickCount64());
    sample.dt_ms = dt_ms;
    sample.atw_err_deg = atw_err;
    sample.border_px = border_px;
    sample.frac = frac;
    sample.synth_us = synth_us;
    sample.wait_us = wait_us;
    if (q_real) {
        sample.q_real[0] = q_real[0]; sample.q_real[1] = q_real[1];
        sample.q_real[2] = q_real[2]; sample.q_real[3] = q_real[3];
    }
    if (q_synth) {
        sample.q_synth[0] = q_synth[0]; sample.q_synth[1] = q_synth[1];
        sample.q_synth[2] = q_synth[2]; sample.q_synth[3] = q_synth[3];
    }
}

inline std::string read_ini_flat(const std::filesystem::path& dir) {
    std::string out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir / L"ofxr_bridge.ini").c_str(), L"rb") == 0 && f) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.empty() || l[0] == ';') continue;
            out += l;
            out += " | ";
        }
        std::fclose(f);
    }
    return out;
}

inline void writer_thread() noexcept {
    auto& s = st();
    std::uint64_t last_frames = 0, last_synth = 0, last_errors = 0;
    ULONGLONG last_line = GetTickCount64();
    while (!s.stop.load(std::memory_order_relaxed)) {
        Sleep(250);
        if (s.rec) FlushViewOfFile(s.rec, sizeof(PhaseRecord));
        const ULONGLONG now = GetTickCount64();
        if (now - last_line < 1000) continue;
        const double dt = static_cast<double>(now - last_line) / 1000.0;
        last_line = now;
        const auto f = s.frames.load(), y = s.synth.load(), e = s.errors.load();
        std::vector<std::uint32_t> v;
        v.reserve(State::kRing);
        for (auto& x : s.end_frame_us) {
            const auto u = x.load(std::memory_order_relaxed);
            if (u) v.push_back(u);
        }
        std::uint32_t p50 = 0, p99 = 0, mx = 0;
        if (!v.empty()) {
            std::sort(v.begin(), v.end());
            p50 = v[v.size() / 2];
            p99 = v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * 0.99))];
            mx = v.back();
        }
        const float hmd_hz = static_cast<float>(s.hmd_hz_milli.load(std::memory_order_relaxed)) / 1000.0f;
        const auto mult = s.current_mult.load(std::memory_order_relaxed);
        const double app_fps = (f - last_frames) / dt;
        const double synth_fps = (y - last_synth) / dt;
        const double delivered_fps = app_fps + synth_fps;
        logf("STAT hmd=%.1fHz app_fps=%.1f mult=%u delivered=%.1f synth/s=%.1f endframe_ms p50=%.2f p99=%.2f max=%.2f wait_ms=%.2f synth_ms=%.2f errs+=%llu phase=%s frame=%llu",
             hmd_hz, app_fps, mult, delivered_fps, synth_fps,
             p50 / 1000.0, p99 / 1000.0, mx / 1000.0,
             s.last_wait_us.load(std::memory_order_relaxed) / 1000.0,
             s.last_synth_us.load(std::memory_order_relaxed) / 1000.0,
             static_cast<unsigned long long>(e - last_errors),
             s.rec ? phase_name(s.rec->phase) : "?", static_cast<unsigned long long>(f));
        last_frames = f; last_synth = y; last_errors = e;
        for (auto& x : s.end_frame_us) x.store(0, std::memory_order_relaxed);  // per-second window
        if (s.log != INVALID_HANDLE_VALUE) FlushFileBuffers(s.log);  // background thread only

        // Flush newly recorded flight samples to reprovr_flight.bin
        const std::uint32_t cur_flight = s.flight_pos.load(std::memory_order_relaxed);
        if (cur_flight > s.flight_flushed && !s.layer_dir.empty()) {
            if (s.flight_file == INVALID_HANDLE_VALUE) {
                const std::wstring p = s.layer_dir + L"\\reprovr_flight.bin";
                s.flight_file = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (s.flight_file != INVALID_HANDLE_VALUE) {
                while (s.flight_flushed < cur_flight) {
                    const std::uint32_t idx = s.flight_flushed % State::kFlightSamples;
                    DWORD written = 0;
                    WriteFile(s.flight_file, &s.flight_ring[idx], sizeof(FrameFlightSample), &written, nullptr);
                    s.flight_flushed++;
                }
                FlushFileBuffers(s.flight_file);
            }
        }
    }
}

// Call once at session creation. `dir` = layer directory.
inline void start(const std::filesystem::path& dir, const char* extra_header) noexcept {
    auto& s = st();
    int expected = 0;
    if (!s.enabled.compare_exchange_strong(expected, 2)) return;  // 2 = initialising
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(dir / L"repro_vr_installed.manifest", ec)) {
        s.enabled.store(-1);
        return;
    }
    const std::uint32_t run_id = static_cast<std::uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() << 16));
    s.log = CreateFileW((dir / L"reprovr_blackbox.log").c_str(), FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE f = CreateFileW((dir / L"reprovr_phase.bin").c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        s.map = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, sizeof(PhaseRecord), nullptr);
        CloseHandle(f);
        if (s.map) s.rec = static_cast<PhaseRecord*>(MapViewOfFile(s.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PhaseRecord)));
    }
    if (s.rec) {
        s.rec->magic = 0x31424252;  // 'RBB1'
        s.rec->run_id = run_id;
    }
    char mod[MAX_PATH]{};
    GetModuleFileNameA(nullptr, mod, MAX_PATH);
    logf("RUN_START run_id=%08X pid=%lu exe=%s build=%s %s", run_id, GetCurrentProcessId(), mod, __DATE__,
         __TIME__);
    if (extra_header) logf("HEADER %s", extra_header);
    logf("INI %s", read_ini_flat(dir).c_str());
    s.enabled.store(1);
    s.layer_dir = dir.wstring();
    set_phase(Phase::session_created);
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { writer_thread(); return 0; }, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// RAII timer for layer_end_frame_impl.
struct EndFrameScope {
    LARGE_INTEGER t0{};
    bool on;
    EndFrameScope() noexcept : on(st().enabled.load(std::memory_order_relaxed) > 0) {
        if (!on) return;
        QueryPerformanceCounter(&t0);
        st().frames.fetch_add(1, std::memory_order_relaxed);
        set_phase(Phase::end_frame_begin);
    }
    ~EndFrameScope() {
        if (!on) return;
        LARGE_INTEGER t1{}, fq{};
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        auto& s = st();
        const auto us = static_cast<std::uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart) + 1;
        s.end_frame_us[s.ring_pos.fetch_add(1, std::memory_order_relaxed) % State::kRing].store(
            us, std::memory_order_relaxed);
        set_phase(Phase::end_frame_end);
    }
};

}  // namespace xrfg::bb
