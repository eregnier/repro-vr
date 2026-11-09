# ReproVR — Universal 6-DOF Frame Synthesis & Forward Reprojection OpenXR Layer

[![License: LGPL v3](https://img.shields.io/badge/License-LGPL_v3-blue.svg)](LICENSE)
[![Release: v1.0.0](https://img.shields.io/badge/Release-v1.0.0--butter--smooth-green.svg)](https://github.com/eregnier/repro-vr/releases/tag/v1.0.0)
[![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-lightgrey.svg)]()

**ReproVR** is a high-performance, non-intrusive OpenXR implicit API layer that provides real-time **2X and 3X stereoscopic frame synthesis** for PC VR games and simulators. 

Engineered as a clean-room implementation inspired by modern forward reprojection principles (such as SpaceWarp architectures), it synthesizes smooth intermediate frames directly on the GPU using hardware optical flow and motion vectors. 

> In terms of motion smoothness and visual comfort, the result feels — to my eyes — every bit as good as Meta's native reprojection, but completely **PCVR and runtime agnostic** (SteamVR, Virtual Desktop, Pimax Play, Meta Link).

---

### 🌟 Key Highlights

* **Single-Source Forward Reprojection**:
  * Warps strictly the latest rendered frame forward in time, eliminating the temporal ghosting, edge doubling, and UI trailing typically caused by legacy 50/50 crossfade blending.
* **Temporal Pose Continuity (Zero-Jitter Tracking)**:
  * Synthetic frame poses remain strictly aligned to the active headset tracking pose (`alpha = 1.0F`), leaving fine micro-rotation compensation to the runtime's Asynchronous Timewarp (ATW). This eliminates back-and-forth oscillation during rapid head turns.
* **Luma Shield UI & Text Protection**:
  * An automated luminance guard detects sharp, high-contrast HUD elements and text against dark moving backgrounds, preventing optical flow block bleeding and keeping in-game text crisp and readable.
* **High-Precision Frame Pacing**:
  * Hardware-timed release throttling locks your game engine to exact fractions of your headset refresh rate (e.g. 45 FPS @ 90 Hz, 60 FPS @ 120 Hz, 40 FPS @ 120 Hz for 3X), providing a consistent, stutter-free cadence.
* **DLSS Swapper-Style Setup Utility (`reprovr_setup.exe`)**:
  * Portable, lightweight Direct2D management app.
  * Automatically discovers installed VR games across your Steam libraries.
  * One-click **Install** and **Uninstall** per game.
  * Real-time settings adjustment with hot-reloading while the game is running.
* **Pure Clean-Room Implementation**:
  * Zero proprietary or copyrighted binary dependencies. Pure C++20 and HLSL shaders.

---

### 🎮 Hardware & Compatibility

* **OS**: Windows 10 / 11 (64-bit).
* **Graphics APIs**: DirectX 11 (via zero-copy interop) and DirectX 12.
* **OpenXR Runtimes**: 
  * SteamVR (Valve Index, HTC Vive, Bigscreen Beyond).
  * Virtual Desktop (Meta Quest 3, Quest Pro, Quest 2).
  * Pimax Play / PimaxXR (Crystal, Crystal Light, Dream Air, 8KX).
  * Meta Quest Link / AirLink.
* **Motion Vector & Flow Backends**:
  * **NVIDIA OFA (Optical Flow Accelerator)**: Turing, Ampere, Ada Lovelace, Blackwell GPUs (extensively tested and verified).
  * **AMD FidelityFX / Optical Flow**: Universal fallback for all modern GPUs (*Note: fully implemented via FidelityFX SDK v1.1.4, but not extensively tested in-game on real AMD hardware — feedback is welcome!*).
  * **DLSS Game Motion Vectors**: Automatic extraction when available in supported titles.

---

### 🚀 Getting Started

#### 1. Download & Extract
Download **`ReproVR-v1.0.0-butter-smooth.zip`** from the [GitHub Releases](https://github.com/eregnier/repro-vr/releases/tag/v1.0.0). Extract the folder anywhere you like.

#### 2. Run the Manager
Launch **`reprovr_setup.exe`**:
* Discovered Steam VR games will automatically appear in the left list.
* If your game or simulator is not listed (e.g. standalone simulator or custom path), click **Add Game** and select the folder containing the game executable.

#### 3. Install
* Select the game from the list and click **Install**.
* ReproVR will place the implicit layer DLL and manifest beside the game binary.
* You can adjust settings (2X / 3X, pacing, overlay, optical flow preset) directly in the GUI.

#### 4. Play
Start your game normally! The layer automatically activates when the game initializes OpenXR.

---

### ⚙️ Live Tuning & Configuration

Settings can be changed before launching or **on the fly while playing** via `reprovr_setup.exe` (or by editing `repro_vr.ini` in the game directory):

| Setting | Options | Description |
| --- | --- | --- |
| `enabled` | `on` / `off` | Master toggle for frame synthesis. |
| `backend` | `FidelityFX` / `NVIDIA OFA` | Optical flow calculation engine. |
| `triple_frame_gen` | `2 images (2X)` / `3 images (3X)` | 2X synthesizes 1 frame per real frame; 3X synthesizes 2 frames. |
| `target_app_fps` | `auto`, `45`, `60`, `72`, `90` | Locks engine presentation cadence to prevent frame pacing judder. |
| `stereo_input_mode` | `auto`, `native`, `AER` | Native stereo vs Alternate Eye Rendering (AER). |
| `overlay` | `off`, `TL`, `TR`, `BL`, `BR` | On-screen HUD displaying real vs synthesized frame rates. |

---

### 📢 Honest Performance Expectations

* **Perceptual Smoothness**: In demanding titles (Microsoft Flight Simulator, DCS World, heavily modded Skyrim VR, Subnautica, UEVR titles), synthesizing frames doubles effective motion smoothness with virtually zero ghosting on the "butter-smooth" pipeline.
* **Inference Overhead**: Hardware optical flow execution consumes ~1.0–2.0 ms of GPU time. For best results, configure your game so that your GPU has ~15–20% headroom at the halved base frame rate.
* **Hardware Testing**: Thoroughly tested and tuned on NVIDIA RTX cards. The AMD FidelityFX fallback path is fully coded in the pipeline but has not been tested on real AMD hardware yet.

---

### 🛡️ Safety Note: VirusTotal Verification

We believe in complete transparency. Whether downloading ReproVR from our releases or any third-party modding repository, we always encourage users to submit the binaries to [VirusTotal.com](https://www.virustotal.com/) for complete peace of mind.

---

### 📜 License

ReproVR is licensed under the [GNU Lesser General Public License v3.0](LICENSE).
