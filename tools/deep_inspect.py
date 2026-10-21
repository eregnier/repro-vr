import struct
import numpy as np

raw = open(r"D:\Games\AFOP\reprovr_flight.bin", "rb").read()
SAMPLE_SIZE = 68
n = len(raw) // SAMPLE_SIZE
print(f"Total samples: {n}")

samples = [struct.unpack("<QIffffII4f4f", raw[i*SAMPLE_SIZE:(i+1)*SAMPLE_SIZE]) for i in range(n)]

frame = np.array([s[0] for s in samples])
tick = np.array([s[1] for s in samples])
dt = np.array([s[2] for s in samples])
atw_err = np.array([s[3] for s in samples])
border = np.array([s[4] for s in samples])
frac = np.array([s[5] for s in samples])

q_real = np.array([[s[8], s[9], s[10], s[11]] for s in samples])
q_synth = np.array([[s[12], s[13], s[14], s[15]] for s in samples])

# Quaternion angular distance between consecutive real frames
def quat_dot(q1, q2):
    return np.abs(np.sum(q1 * q2, axis=-1))

dots = quat_dot(q_real[:-1], q_real[1:])
dots = np.clip(dots, -1.0, 1.0)
angles_deg = 2.0 * np.arccos(dots) * (180.0 / np.pi)

print(f"Angle between consecutive frames:")
print(f"  mean = {angles_deg.mean():.4f}° | p50 = {np.percentile(angles_deg, 50):.4f}° | p95 = {np.percentile(angles_deg, 95):.4f}° | max = {angles_deg.max():.4f}°")

# Correlate border excursion with head rotation
print(f"\nBorder excursion stats:")
print(f"  p50 = {np.percentile(border, 50):.1f}px")
print(f"  p75 = {np.percentile(border, 75):.1f}px")
print(f"  p90 = {np.percentile(border, 90):.1f}px")
print(f"  p95 = {np.percentile(border, 95):.1f}px")
print(f"  p99 = {np.percentile(border, 99):.1f}px")

# Print 10 samples with largest border
big_idx = np.argsort(border)[-10:]
print("\nTop 10 largest border frames:")
for idx in big_idx:
    print(f"  Frame {frame[idx]}: border={border[idx]:.1f}px atw_err={atw_err[idx]:.4f} deg, frac={frac[idx]:.3f}, q_real={q_real[idx]}")

# Consecutive frames difference
consecutive_diff = np.diff(frame)
print(f"\nFrame index deltas: min={consecutive_diff.min()}, max={consecutive_diff.max()}, unique={np.unique(consecutive_diff)}")
