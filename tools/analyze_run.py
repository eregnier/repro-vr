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
for l in run:
    m = re.search(r"STAT fps=([\d.]+) synth/s=([\d.]+) endframe_ms p50=([\d.]+) p99=([\d.]+) max=([\d.]+) errs\+=(\d+) phase=(\w+) frame=(\d+)", l)
    if m:
        stats.append((l[:12], float(m[1]), float(m[2]), float(m[3]), float(m[4]), float(m[5]), int(m[6]), m[7], int(m[8])))
errs = [l for l in run if " ERROR " in l]

rec = d / "reprovr_phase.bin"
if rec.exists() and rec.stat().st_size >= 64:
    magic, run_id, phase, hr, frame, tick, synth, nerr, tid = struct.unpack("<IIIIQQQII", rec.read_bytes()[:56])
    print(f"\nLAST PHASE (mapped): {PHASES[phase] if phase < len(PHASES) else phase}  frame={frame}  "
          f"synth={synth}  last_hr=0x{hr:08X}  errors={nerr}  tid={tid}  run_id={run_id:08X}")

if stats:
    fps = [s[1] for s in stats]
    p99 = [s[4] for s in stats]
    mx = [s[5] for s in stats]
    print(f"\nSTATS over {len(stats)} s: fps avg={sum(fps)/len(fps):.1f} min={min(fps):.1f} | "
          f"EndFrame p99 avg={sum(p99)/len(p99):.2f}ms worst-max={max(mx):.1f}ms | "
          f"synth/s avg={sum(s[2] for s in stats)/len(stats):.1f}")
    spikes = [s for s in stats if s[5] > 50]
    if spikes:
        print(f"  {len(spikes)} s with EndFrame spike >50 ms, first at {spikes[0][0]} (phase {spikes[0][7]})")
    last = stats[-1]
    print(f"  last STAT at {last[0]} phase={last[7]} frame={last[8]}")
print(f"ERRORS: {len(errs)}" + ("".join("\n  " + e[:200] for e in errs[:5])))

# verdict
print("\nVERDICT:")
if not stats:
    print("  - Never produced a STAT line: died/hung within the first second after session creation.")
elif stats[-1][1] < 1:
    print(f"  - fps collapsed to ~0 at the end -> HANG in phase '{stats[-1][7]}' (see LAST PHASE).")
if errs:
    print("  - HRESULT errors present: fix those first (first error line above).")
if stats and sum(fps) / len(fps) < 40:
    print("  - Low fps: compare EndFrame p99 vs synth/s; p99 high => layer cost, low => app-bound.")
if stats and not errs and stats[-1][1] >= 1:
    print("  - Clean run.")
