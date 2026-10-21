"""ReproVR run verdict. Usage: uv run python tools/analyze_run.py [game_dir=D:\\Games\\AFOP]

Reads reprovr_phase.bin (crash-surviving last phase) + reprovr_blackbox.log (per-second
stats) and prints a short verdict for the LAST run in the log.
"""
import re
import struct
import sys
from pathlib import Path

PHASES = ["none", "session_created", "end_frame_begin", "capture", "synth_prime_submit",
          "synth_pair_submit", "synth_done", "publish", "runtime_end_frame", "end_frame_end"]
d = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\Games\AFOP")
log = d / "reprovr_blackbox.log"
if not log.exists():
    sys.exit("no reprovr_blackbox.log (layer not deployed with repro_vr_installed.manifest, or never reached session)")

lines = log.read_text(errors="replace").splitlines()
starts = [i for i, l in enumerate(lines) if "RUN_START" in l]
run = lines[starts[-1]:] if starts else lines
print("\n".join(l[:230] for l in run[:3]))

stats = []
# New format regex:
# STAT hmd=90.0Hz app_fps=45.0 mult=2 delivered=90.0 synth/s=45.0 endframe_ms p50=12.2 p99=14.5 max=16.0 wait_ms=8.0 synth_ms=3.2 errs+=0 phase=synth_done frame=123
re_new = re.compile(
    r"STAT hmd=([\d.]+)Hz app_fps=([\d.]+) mult=(\d+) delivered=([\d.]+) synth/s=([\d.]+) "
    r"endframe_ms p50=([\d.]+) p99=([\d.]+) max=([\d.]+) wait_ms=([\d.]+) synth_ms=([\d.]+) "
    r"errs\+=(\d+) phase=(\w+) frame=(\d+)"
)
re_old = re.compile(
    r"STAT fps=([\d.]+) synth/s=([\d.]+) endframe_ms p50=([\d.]+) p99=([\d.]+) max=([\d.]+) "
    r"errs\+=(\d+) phase=(\w+) frame=(\d+)"
)

for l in run:
    m = re_new.search(l)
    if m:
        # (time, hmd_hz, app_fps, mult, delivered, synth_s, p50, p99, max, wait_ms, synth_ms, errs, phase, frame)
        stats.append({
            "t": l[:12], "hmd": float(m[1]), "app_fps": float(m[2]), "mult": int(m[3]),
            "delivered": float(m[4]), "synth_s": float(m[5]), "p50": float(m[6]),
            "p99": float(m[7]), "max": float(m[8]), "wait_ms": float(m[9]),
            "synth_ms": float(m[10]), "errs": int(m[11]), "phase": m[12], "frame": int(m[13])
        })
    else:
        m2 = re_old.search(l)
        if m2:
            stats.append({
                "t": l[:12], "hmd": 0.0, "app_fps": float(m2[1]), "mult": 2,
                "delivered": float(m2[1]) + float(m2[2]), "synth_s": float(m2[2]),
                "p50": float(m2[3]), "p99": float(m2[4]), "max": float(m2[5]),
                "wait_ms": 0.0, "synth_ms": 0.0, "errs": int(m2[6]), "phase": m2[7], "frame": int(m2[8])
            })

errs = [l for l in run if " ERROR " in l]

rec = d / "reprovr_phase.bin"
if rec.exists() and rec.stat().st_size >= 64:
    raw = rec.read_bytes()
    magic, run_id, phase, hr, frame, tick, synth, nerr, tid = struct.unpack("<IIIIQQQII", raw[:48])
    hmd_hz = 0.0
    mult = 2
    wait_us = 0
    synth_us = 0
    if len(raw) >= 64:
        hmd_milli, mult, pad, wait_us, synth_us = struct.unpack("<IIHHII", raw[40:64])[:5] if False else struct.unpack("<IIHHII", raw[44:64]) if False else (0, 2, 0, 0, 0)
    print(f"\nLAST PHASE (mapped): {PHASES[phase] if phase < len(PHASES) else phase}  frame={frame}  "
          f"synth={synth}  last_hr=0x{hr:08X}  errors={nerr}  tid={tid}  run_id={run_id:08X}")

if stats:
    app_fps = [s["app_fps"] for s in stats]
    p99 = [s["p99"] for s in stats]
    p50 = [s["p50"] for s in stats]
    mx = [s["max"] for s in stats]
    synth_ms = [s["synth_ms"] for s in stats]
    wait_ms = [s["wait_ms"] for s in stats]
    hmd = max(s["hmd"] for s in stats)
    delivered = [s["delivered"] for s in stats]
    mult = stats[-1]["mult"]

    avg_app = sum(app_fps) / len(app_fps)
    avg_deliv = sum(delivered) / len(delivered)
    avg_p99 = sum(p99) / len(p99)
    avg_p50 = sum(p50) / len(p50)
    avg_synth = sum(synth_ms) / len(synth_ms)
    avg_wait = sum(wait_ms) / len(wait_ms)

    print(f"\nSTATS over {len(stats)} s:")
    print(f"  HMD: {hmd:.1f} Hz (target) | Multiplier: {mult}X")
    print(f"  Game: {avg_app:.1f} FPS (min {min(app_fps):.1f}) -> Delivered: {avg_deliv:.1f} FPS")
    if hmd > 0:
        sync_pct = min(100.0, (avg_deliv / hmd) * 100.0)
        gap = abs(avg_deliv - hmd)
        print(f"  Cadence Sync: {sync_pct:.1f}% ({gap:.1f} FPS delta to HMD target)")
    print(f"  EndFrame Total: p50={avg_p50:.2f}ms | p99={avg_p99:.2f}ms | max={max(mx):.1f}ms")
    if avg_synth > 0 or avg_wait > 0:
        print(f"    -> Synthesis GPU/Submit: {avg_synth:.2f}ms")
        print(f"    -> Runtime Wait/Pacing:  {avg_wait:.2f}ms")

    spikes = [s for s in stats if s["max"] > 50]
    if spikes:
        print(f"  {len(spikes)} s with EndFrame spike >50 ms, first at {spikes[0]['t']} (phase {spikes[0]['phase']})")
    last = stats[-1]
    print(f"  last STAT at {last['t']} phase={last['phase']} frame={last['frame']}")

print(f"ERRORS: {len(errs)}" + ("".join("\n  " + e[:200] for e in errs[:5])))

# verdict
print("\nVERDICT:")
if not stats:
    print("  - Never produced a STAT line: died/hung within the first second after session creation.")
elif stats[-1]["app_fps"] < 1:
    print(f"  - fps collapsed to ~0 at the end -> HANG in phase '{stats[-1]['phase']}' (see LAST PHASE).")
if errs:
    print("  - HRESULT errors present: fix those first (first error line above).")
if stats and hmd > 0:
    if abs(avg_deliv - hmd) < 3.0:
        print(f"  - [PERFECT] Delivered rate matches HMD refresh rate ({avg_deliv:.1f} ~= {hmd:.1f} Hz). Zero cadence judder.")
    elif avg_deliv < hmd * 0.9:
        print(f"  - [CADENCE MISMATCH] Delivered {avg_deliv:.1f} FPS vs {hmd:.1f} Hz HMD target -> causes judder/stutter.")
        if mult == 2 and avg_app < hmd * 0.45:
            print(f"    -> App ({avg_app:.1f} FPS) is below half of HMD refresh ({hmd/2:.1f} FPS). Need more GPU headroom or lower settings.")
if stats and not errs and stats[-1]["app_fps"] >= 1:
    print("  - Clean run (no crash, no HRESULT error).")
