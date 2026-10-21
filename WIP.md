# WIP — ReproVR (`C:\code\repro-vr`)

> Document de reprise après compaction. Contient : objectif, état des branches Git, architecture Phase 2, rétro-ingénierie Quest 3 / SteamVR / Luke Ross (`RealVR64.dll` + `dxgi2.dll`), déploiement dans *Avatar: Frontiers of Pandora* (`D:\Games\AFOP`), diagnostics des 3 runs en jeu et prochaines étapes.
> Dernière mise à jour : **2026-10-20 13:40** (`master` @ `da9a27b`, `clean-room-mit` @ `8138f0b`).

---

## 1. Objectif & Dépôt Git

* **Projet** : `C:\code\repro-vr` — Layer OpenXR universel de synthèse de frames spatio-temporelle 6-DOF, extrapolation ASW/AppSW et synthèse stéréo croisée pour PCVR (cible matérielle : **NVIDIA RTX 5090**, 32 Go VRAM, Pimax / Quest 3).
* **Remote GitLab** : `git@gitlab.com:eregnier/repro-vr.git`
* **Branches poussées et synchronisées** :
  1. **`master` (`e4f8b66`)** : Version complète multi-API (D3D12 natif + ponts D3D11 & Vulkan) et multi-runtime (SteamVR, VDXR, Pimax) intégrant toute la **Phase 2** et les correctifs d'interopérabilité Luke Ross (`RealVR64.dll` / `dxgi2.dll`). **98 / 98 tests CTest passent (100 %)**.
  2. **`clean-room-mit` (`8138f0b`)** : Réécriture **100 % Clean-Room sous double licence `MIT OR Apache-2.0`** (~7 600 lignes C++20/HLSL SM 5.1, zéro code LGPLv3/GPLv3 hérité d'OFXR). Inclut : `pose`, `depth_reprojection`, `reprojection_telemetry`, `d3d12_history` (capture mip 0), `d3d12_frame_synthesizer` (`submit_prime`, `submit_pair`, `submit_pose_centered_pair`, `submit_extrapolated_pair`, `submit_cross_eye_stereo`), `upscaler_auto_hook` (interception in-process NGX/DLSS), `version_proxy` (`version.dll` drop-in), et `openxr_layer` (synthèse D3D12 2X avec swapchains privés `mipCount=1` et soumission double `S` à $\mathbf{P}_S$ + `B` à $\mathbf{P}_B$). **6 / 6 suites de tests GPU/OpenXR passent (100 %)**.

---

## 2. Rétro-Ingénierie & Ressources Locales

* **Dump Quest 3 (Horizon OS v71+, strictement read-only)** :
  * Stocké dans `C:\code\repro-vr\docs\quest3_dump\` (**gitignored**, ne jamais commiter).
  * Contient les APKs/libs arm64 (`libvrruntimeservice.so`, `libvrcompositor.so`, `libxrstreamingclient.so`), 1 006 CVars, 9 932 classes RTTI, et **101 shaders GLSL extraits en clair** (dont l'Uber-Shader PTW `71_frag_PTW_PositionalTimeWarp_956ee89f.glsl` de 2 357 lignes).
* **Shaders SteamVR "Smooth Motion / Frame Hallucination"** :
  * 43 shaders `.fxo` désassemblés dans `docs/quest3_dump/analysis/steamvr_shaders/` (`camera_motion_backout_cs`, `mv_static_reject_cs`, `mv_depth_consistency_cs`, `mv_distrust_dilate_cs`, `mv_repair_cs` 16-tap ring, `mv_dilation_3x3/5x5/7x7`).
* **Outil REA (`C:\code\rea`)** :
  * Disponible localement + scripts rapides `uv run --with pefile --with capstone python` pour désassembler à la volée les binaires tiers (`RealVR64.dll`, `dxgi2.dll`, `OVRServer_x64.exe`, `PDAFWPlugin.dll`).

---

## 3. Ce que la Phase 2 résout (et où c'est câblé dans le code)

1. **Suppression des micro-saccades à 45 Hz lors des rotations de tête & du saut des bords noirs** :
   * *Cause initiale* : La frame synthétique $S$ (affichée à mi-chemin temporel $\mathbf{P}_S$) était construite dans le frustum de la frame finale $\mathbf{P}_B$ (`submit_pair` avec `target_views = current_source_views`) et soumise au casque avec `view.pose = B.pose`. L'ATW du casque tirait donc la frame $S$ de $-\frac{1}{2}\Delta\theta$ en arrière une frame sur deux.
   * *Solution Phase 2* :
     * `prepare_projection_frame` (`src/layer/openxr_layer.cpp`) calcule la pose interpolée $\mathbf{P}_S = \text{extrapolate\_pose}(\mathbf{P}_A, \mathbf{P}_B, \alpha)$.
     * `prepare_frame_generation` appelle `synthesizer->submit_pose_centered_pair(...)` (flags `(1U << 28U) | 64U | 128U`).
     * `src/d3d12/d3d12_frame_synthesizer.cpp` peuple `PreviousMappings[view_idx]` ($\mathbf{T}_{S \to A}$) et `PreviousMappings[1 - (view_idx & 1)]` ($\mathbf{T}_{S \to B}$) avec bit 26 + bit 28.
     * `src/d3d12/frame_synthesis.hlsl` applique le shift symétrique `asw_cam_shift = (previous_coverage.coordinate - pixel) * (-frac / max(1.0 - frac, 0.25))` et clamp-to-edge dans `sample_current_target_with_asw`.
     * `build_generated_frame_end_info` soumet la frame synthétique au runtime avec `view.pose = synth_pose` ($\mathbf{P}_S$).
     * *Résultat mesuré en jeu dans Avatar (`reprovr_live.log`)* : **`atw_err = 0.000deg`** et **temps GPU RTX 5090 = `1,46 ms` par paire** (`Pack=31 µs`, `NVOF=1,27 ms`, `Composition=152 µs`).
2. **Qualité bords / silhouettes / HUD** :
   * `include/xrfg/depth_reprojection.hpp` + `src/core/depth_reprojection.cpp` + `src/d3d12/frame_synthesis.hlsl` : `analyze_depth_neighborhood_3x3` (`sample_current_depth_neighborhood`), `compute_static_motion_damping_factor` (`static_motion_damping`), `repair_distrusted_motion_vector_ring`, `compute_edge_feather_weight` (`edge_feather_factor`).
3. **Télémétrie Lock-Free & Hot-Reload (`include/xrfg/reprojection_telemetry.hpp`, `src/core/reprojection_telemetry.cpp`)** :
   * Chargée au démarrage de session (`poll_hot_reload(current_layer_directory(), true)` dans `layer_create_session_impl`) et relue à 1 Hz si activée.
   * Trace légère temps réel dans le dossier du jeu : `D:\Games\AFOP\reprovr_live.log` (activée automatiquement dès que `repro_vr_installed.manifest` est présent, écrit uniquement l'init + les 16 premières frames + 1 ligne toutes les 300 frames + toute erreur éventuelle, sans aucun `FlushFileBuffers`).

---

## 4. Déploiement dans *Avatar: Frontiers of Pandora* (`D:\Games\AFOP`) & Découvertes Luke Ross (`RealVR64.dll` / `dxgi2.dll`)

### 4.1 Architecture d'Avatar avec Luke Ross REAL VR
* `afop.exe` tourne en **D3D12** avec `dxgi2.dll` (le fork ReShade de Luke Ross) + `RealVR64.dll` + `CheekyFoveatedDLSS` (`dxgi.dll` $\to$ `dxgi2.dll`).
* `RealVR64.dll` crée une session OpenXR **D3D11** (`XR_TYPE_GRAPHICS_BINDING_D3D11_KHR`), convertie en session **D3D12** par notre pont `d3d11_bridge=1`.

### 4.2 Les 4 Problèmes Rencontrés en Live & Leurs Correctifs (`master` @ `e4f8b66`)

1. **Erreur `-36 (XR_ERROR_API_LAYER_NOT_PRESENT)` au tout premier lancement** :
   * *Causes* :
     1. Le manifeste JSON avait des `\` non échappés.
     2. Le loader OpenXR embarqué dans `RealVR64.dll` exige le champ `"disable_environment"` dans le JSON.
     3. En mode **High Integrity (Admin)** (vérifié dans `RealVR64.dll` à RVA `0x3b9350` via `GetTokenInformation(TokenIntegrityLevel)`), le loader OpenXR ignore `XR_API_LAYER_PATH` et ne lit que `HKLM/HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Explicit`.
   * *Correctif (`src/proxy/version_proxy.cpp`, commit `760bcc5`)* : `version.dll` régénère toujours un JSON propre avec `"disable_environment": "XRFG_DISABLE_OFXR_BRIDGE"` et enregistre automatiquement le manifeste dans `HKCU` et `HKLM\...\ApiLayers\Explicit`.

2. **Crash `DXGI_ERROR_DEVICE_REMOVED` avec `keep_synthetic_depth=1`** :
   * *Cause* : Sous `d3d11_bridge=1`, le swapchain de profondeur de Luke Ross utilise `D3D11BridgePath::depth_private` (ses textures D3D12 côté runtime ne sont jamais écrites ; sur la frame réelle, `strip_private_depth_info` retire `view.next`). Laisser `view.next != nullptr` sur `first_generated` envoyait un `depth_private` non initialisé à SteamVR.
   * *Correctif (`src/layer/openxr_layer.cpp`, commit `5232e19`)* : `view.next = nullptr` est toujours forcé sur `synthetic == true`.

3. **Ralentissements extrêmes (6,7 FPS) & pics de ~1 seconde lors du Run 2 (`13:10`)** :
   * *Causes prouvées par `ofxr-bridge-flight-20261020-130958-pid27076.log`* :
     1. `flush_each_event=1` dans `ofxr_bridge.ini` appelait `FlushFileBuffers()` ~35 fois par frame, déclenchant des pauses matérielles NVMe de **90 à 95 ms** toutes les ~8 écritures (jusqu'à **1 248 ms** par `xrEndFrame`).
     2. `low_latency_input=1` avançait `predictedDisplayTime` de `+1 ns` quand l'horloge était en avance, ce qui cassait le calcul `predictedDisplayTime / predictedDisplayPeriod` de Luke Ross (`WARN | Discontinuity in input frame counter: have 457, expected 458` + `Frame parity mismatch / eyes swapped`).
   * *Correctif (`D:\Games\AFOP\ofxr_bridge.ini`)* : `logging_enabled=0`, `flush_each_event=0`, et `low_latency_input=0` (avec `deep_pipeline=0` qui assure déjà la basse latence sans toucher au multiple de `predictedDisplayPeriod`).

4. **Crash `DXGI_ERROR_DEVICE_REMOVED` à l'initialisation lors du Run 3 (`13:16:12`)** :
   * *Cause prouvée par désassemblage de `dxgi2.dll` (RVA `0x94fe2`, `0x9a0c0`, `0x9ae10`)* :
     * Quand `create_d3d12_device_for_d3d11` (`src/d3d11/d3d11_d3d12_interop.cpp`) appelle `D3D12CreateDevice`, `dxgi2.dll` (le hook ReShade de Luke Ross) intercepte l'appel et retourne **son proxy C++ `D3D12Device`** (`WARN | We already have a runtime for the same device (returning existing proxy ...)`).
     * Dès lors, chaque `CreateCommandQueue` de notre layer, de SteamVR et de `nvofapi64.dll` (`Type = 6 VIDEO_ENCODE`) passait par `D3D12Device::CreateCommandQueue` de ReShade (`0x9a0c0`), qui wrappait nos queues internes dans des `reshade::d3d12::command_queue_impl` et déclenchait `"Loading image files for textures ..."` / `"Blitter used without being initialized"` en concurrence avec le thread principal du jeu !
   * *Correctif (`src/d3d11/d3d11_d3d12_interop.cpp` & `src/vulkan/vulkan_d3d12_interop.cpp`, commit `e4f8b66`)* :
     * Dans `dxgi2.dll` (`0x9ae10`), `D3D12Device::CreateFence` fait un tail-call direct vers `rcx->_orig->CreateFence(...)` et retourne un `ID3D12Fence` natif de `d3d12core.dll`.
     * Juste après `D3D12CreateDevice`, nous créons un `probe_fence` et appelons `probe_fence->GetDevice(IID_PPV_ARGS(&native_device))`, ce qui retourne **le vrai `ID3D12Device` natif non-wrappé par ReShade (`dxgi2.dll`)** !
     * Ainsi, aucune queue D3D12 interne de ReproVR, NVOF ou SteamVR ne passe plus jamais par les hooks ReShade de `dxgi2.dll`.

---

### 4.3 Run 4 (13:26) : BSOD `0x139` & revert du dé-wrapping

* Le run avec `e4f8b66` (dé-wrapping `probe_fence->GetDevice`) a fini en **BSOD `KERNEL_SECURITY_CHECK_FAILURE 0x139` (param1=3) à ~13:29:44** (dump généré ; event `nvlddmkm 153` déjà vu à 12:52).
* Logs : `reprovr_live.log` s'arrête à `SYNTH #2 kind=PRIME` (13:26:31), `RealVR64.log` à `session VISIBLE` : blocage dès la 2e frame synthétique, ~3 min avant le BSOD. Une `CreateCommandQueue` passait encore par le proxy ReShade (autre thread) : le dé-wrapping ne réglait rien.
* `5232e19` n'avait pas crashé (juste lent, causes corrigées via l'ini). **Hypothèse** : mélange objets wrappés (ressources partagées D3D11/ReShade) et natifs (nos queues + NVOF `VIDEO_ENCODE`) sans synchronisation → corruption d'état GPU/driver. **Non confirmée** (analyser `C:\Windows\MEMORY.DMP`).
* **Action** : commit `3834cae` revert le dé-wrapping dans `d3d11_d3d12_interop.cpp` / `vulkan_d3d12_interop.cpp` (le gating de `reprovr_live.log` est conservé).

### 4.4 Boîte noire (`da9a27b`) : `src/layer/blackbox.hpp` + `tools/analyze_run.py`

* `reprovr_phase.bin` (mmap 64 o, flush 250 ms par thread de fond) : dernière phase / frame / `HRESULT` survivant à un hang ou BSOD. Phases : `session_created, end_frame_begin, capture, synth_prime_submit, synth_pair_submit, synth_done, publish, runtime_end_frame, end_frame_end`.
* `reprovr_blackbox.log` : en-tête (build, ini, exe, `run_id`) + ligne `STAT` par seconde (fps, `xrEndFrame` p50/p99/max, synth/s, erreurs, phase) + lignes `ERROR hr=...`. Le thread de rendu n'écrit jamais sur disque.
* Actif seulement avec `repro_vr_installed.manifest`. Analyse : `uv run python tools/analyze_run.py D:\Games\AFOP` (verdict : dernière phase, hang, spikes, erreurs, perf).
* **Manque** : timings GPU par étape (NVOF/composition) et VRAM, pas encore dans le log.
* **Piège build** : la DLL à déployer est `build\XR_APILAYER_XRFrameBridge_diagnostic.dll` (pas `build\ofxr\`, copie de test).


### 4.6 Preparation Run 6 : Cure des ghosts & vibrations (NVOF pur + Extrapolated ASW)

* **Diagnostic Run 5** :
  * App a atteint **45.0 FPS stables** (EndFrame p50 passe de 26.5 ms a 20.8 ms).
  * *Ghosts / artefacts* : RealVR64.log a montre des erreurs CUDA repetees (cudaImportExternalMemory (1), cudaCreateSurfaceObject (400), DLSS eyes swapped). Le mode motion_vectors=dlss injectait donc des vecteurs corrompus/inverses dans le warp de ReproVR.
  * *Vibrations / scintillement* : 
vidia_input_scale=33 produisait une grille de vecteurs trop grossiere qui bavait sur les feuillages fins d Avatar. De plus, le mode midpoint en AFR souffrait du dephasage temporel entre l oeil frais et l oeil persistant.
  * *Resolution* : Notee a ~2K par oeil sur Quest 3 (marge plus confortable qu a 4K Pimax, mais les gains architecturaux restent determinants).
* **Ajustements Run 6 (ofxr_bridge.ini)** :
  * motion_vectors=off : NVOF calcule ses propres vecteurs directement a partir des textures de rendu, sans dependre du CUDA defaillant de Luke Ross.
  * 
vidia_input_scale=50 : echelle d Optical Flow restauree a 50% pour un suivi geometrique precis sans scintillement.
  * pose_centering_mode=extrapolated_asw : projection predictive de la pose vers le scanout futur (elimine le retard d interpolation et aligne les yeux sur la rotation du casque).

### 4.5 Preparation Run 5 (aee6ce1) : Cible 90 Hz 2X (Pimax / Quest 3 @ 90 Hz) + Observabilite fine

* **Diagnostic Run 4** : 0 crash, 0 erreur GPU (errors=0, last_hr=0x00000000, 401s sans accroc). Le revert a definitivement tue le BSOD 0x139.
  * *Stutter constate* : Quest 3 etait a 120 Hz, jeu a 36 FPS, mod en 2X force (72 FPS livres sur dalle 120 Hz -> judder permanent a 1.666 frames/scanout).
  * *Latence manette* : xrEndFrame prenait 26.5 ms a lui seul (thread bloque), et l interpolation midpoint retient la frame reelle du joueur pour afficher la synthetique en premier (comportement type FrameGen).
* **Ajustements Run 5** :
  * Casque configure a **90 Hz** par l utilisateur pour simuler fidelement l environnement cible du Pimax a 90 Hz.
  * Cible : **2X strict** (45 FPS jeu x 2 = 90 Hz livres).
  * ofxr_bridge.ini allege pour liberer le GPU et le CPU : nvidia_preset=fast, nvidia_input_scale=33, triple_frame_gen=0.
  * **Observabilite fine deployee** :
    * Boite noire (blackbox.hpp) : decomposition synth_ms (duree GPU NVOF) vs wait_ms (duree bloque dans runtime), detection hmd_hz, multiplicateur mult.
    * tools/analyze_run.py : calcul automatique du % de synchronisation avec la frequence HMD et diagnostic immediat de la source du stutter.
  * Deploye dans D:\Games\AFOP\XR_APILAYER_XRFrameBridge_diagnostic.dll (1 049 088 octets). Logs nettoyes pour le nouveau run.

---

## 5. Fichiers Déployés dans `D:\Games\AFOP`

* D:\Games\AFOP\XR_APILAYER_XRFrameBridge_diagnostic.dll (build master @ ee6ce1)
* `D:\Games\AFOP\version.dll` (proxy bootstrap @ `760bcc5`)
* `D:\Games\AFOP\XR_APILAYER_XRFrameBridge_diagnostic.json` (manifeste valide avec `disable_environment` + enregistré dans `HKCU/HKLM\...\ApiLayers\Explicit`)
* `D:\Games\AFOP\ofxr_bridge.ini` (configuré avec `pose_centering_mode=midpoint`, `vram_profile=lean`, `deep_pipeline=0`, `low_latency_input=0`, `keep_synthetic_depth=0`, `logging_enabled=0`, `flush_each_event=0`)
* `D:\Games\AFOP\switch_reprovr_edition.bat` (permet de basculer entre `[1] Master Phase 2`, `[2] Clean-Room MIT/Apache-2.0`, `[3] Désactivé`)
* Sauvegarde d'origine intacte dans : `D:\Games\AFOP\_reprovr_backup_20261019_204409\`

---

## 6. Commandes de Build & Tests Rapides

Depuis un `.bat` (pour éviter le mangling des chemins par Git-Bash) :
```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d C:\code\repro-vr\build
cmake --build . -j 8
ctest -LE needs_gpu_timing -j 4
ctest -L needs_gpu_timing -j 1
```

---

## 7. Prochaine Étape Immédiate à la Reprise

1. **Tester le build `da9a27b`** (revert dé-wrapping + boîte noire, déjà déployé dans `D:\Games\AFOP`), puis `uv run python tools/analyze_run.py`.
2. **Si BSOD / hang** : lire la dernière phase (`reprovr_phase.bin`), analyser `MEMORY.DMP` (driver fautif), tester sans NVOF (`PRIME` seul).
3. **Si stable** : validation visuelle en casque (micro-saccades, bords, réactivité manette), puis ajouter timings GPU/VRAM à la boîte noire.
4. Portage éventuel sur `clean-room-mit` seulement après validation sur `master`.
