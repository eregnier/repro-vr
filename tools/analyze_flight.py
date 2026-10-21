"""ReproVR Flight Recorder NumPy Analyzer.
Usage: uv run --with numpy python tools/analyze_flight.py [game_dir=D:\\Games\\AFOP]

Parses reprovr_flight.bin and produces an exact kinematic & temporal breakdown.
"""
import struct
import sys
from pathlib import Path
try:
    import numpy as np
except ImportError:
    np = None

d = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\Games\AFOP")
flight_file = d / "reprovr_flight.bin"
if not flight_file.exists():
    sys.exit(f"no flight file found at {flight_file}")

raw = flight_file.read_bytes()
SAMPLE_SIZE = 68
n_samples = len(raw) // SAMPLE_SIZE
if n_samples == 0:
    sys.exit("Flight file empty or incomplete.")

# struct FrameFlightSample {
#     uint64_t frame;
#     uint32_t tick_ms;
#     float dt_ms;
#     float atw_err_deg;
#     float border_px;
#     float frac;
#     uint32_t synth_us;
#     uint32_t wait_us;
#     float q_real[4];
#     float q_synth[4];
# };
fmt = "<QIIffffII4f4f"
samples = []
for i in range(n_samples):
    chunk = raw[i * SAMPLE_SIZE : (i + 1) * SAMPLE_SIZE]
    data = struct.unpack("<QIffffII4f4f", chunk)
    samples.append(data)

frame = [s[0] for s in samples]
tick = [s[1] for s in samples]
dt = [s[2] for s in samples]
atw_err = [s[3] for s in samples]
border = [s[4] for s in samples]
frac = [s[5] for s in samples]
synth_us = [s[6] for s in samples]
wait_us = [s[7] for s in samples]
qx_real = [s[8] for s in samples]
qy_real = [s[9] for s in samples]
qz_real = [s[10] for s in samples]
qw_real = [s[11] for s in samples]

print(f"=== REPROVR FLIGHT ANALYSIS ({n_samples} frames recorded) ===")
duration_s = (tick[-1] - tick[0]) / 1000.0 if n_samples > 1 else 0
print(f"Duration: {duration_s:.1f} s | Frames: {frame[0]} -> {frame[-1]}")

if np is not None:
    atw_arr = np.array(atw_err)
    border_arr = np.array(border)
    synth_arr = np.array(synth_us) / 1000.0
    wait_arr = np.array(wait_us) / 1000.0
    
    print("\n--- KINEMATICS & POSE ALIGNMENT ---")
    print(f"  ATW Pose Error (deg) : avg={atw_arr.mean():.4f}° | p50={np.percentile(atw_arr, 50):.4f}° | p99={np.percentile(atw_arr, 99):.4f}° | max={atw_arr.max():.4f}°")
    print(f"  Border Excursion (px): avg={border_arr.mean():.1f}px | p50={np.percentile(border_arr, 50):.1f}px | p99={np.percentile(border_arr, 99):.1f}px | max={border_arr.max():.1f}px")
    
    jumps = np.where(border_arr > 10.0)[0]
    if len(jumps) > 0:
        print(f"  [WARNING] {len(jumps)} frames with border jump > 10px (first at frame {frame[jumps[0]]}: {border_arr[jumps[0]]:.1f}px)")
    else:
        print("  [PERFECT] 0 border jumps > 10px across the entire flight! Camera poses are rock solid.")
        
    print("\n--- TIMINGS BREAKDOWN (GPU vs Runtime) ---")
    print(f"  NVOF Synthesis GPU: avg={synth_arr.mean():.2f}ms | p99={np.percentile(synth_arr, 99):.2f}ms | max={synth_arr.max():.2f}ms")
    print(f"  Compositor Wait   : avg={wait_arr.mean():.2f}ms | p99={np.percentile(wait_arr, 99):.2f}ms | max={wait_arr.max():.2f}ms")
else:
    avg_atw = sum(atw_err) / len(atw_err)
    max_atw = max(atw_err)
    avg_border = sum(border) / len(border)
    max_border = max(border)
    print(f"  ATW Pose Error: avg={avg_atw:.4f}° | max={max_atw:.4f}°")
    print(f"  Border Excursion: avg={avg_border:.1f}px | max={max_border:.1f}px")
