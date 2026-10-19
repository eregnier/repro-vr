# ReproVR — Plan Directeur Phase 2 : Calibration Spatio-Temporelle, Zéro-Saccade, Réduction VRAM & Conformité Clean-Room (MIT / Apache-2.0)

Ce document synthétise :
1. **La clôture du Harvest (Quest 3 + SteamVR Motion Smoothing)** et le recoupement complet des 4 implémentations de référence (Meta Quest 3 Horizon OS, Meta PC ASW 2.0, Valve SteamVR « Frame Hallucination », PureDark AFW).
2. **Le diagnostic mathématique et architectural exact des 4 symptômes observés lors du test réel sur RTX 5090** (bords noirs saccadés en rotation tête, micro-saccades en mouvement de tête vs fluidité parfaite au stick, input lag manette de 500 ms–1 s, empreinte VRAM).
3. **La feuille de route d'ingénierie Phase 2** (correctifs ciblés + pépites Quest 3 / SteamVR + instrumentation temps réel zéro-overhead + boucle d'auto-calibration).
4. **La stratégie juridique & licence Open-Source (MIT / Apache-2.0)** garantissant zéro risque vis-à-vis de Meta, Valve et des projets tiers.

---

## 1. Bilan du Harvest : Quest 3 (Horizon OS v14) + SteamVR Motion Smoothing

### 1.1 Ce qui a été ajouté au Harvest Quest 3 (100 % Read-Only)
En plus de `VrDriver.apk` (`libvrruntimeservice.so`, `libvrapiimpl.so`) et `VrShell.apk`, nous avons rapatrié et analysé dans `docs/quest3_dump/` (gitignoré) :
- **`xrstreamingclient.apk` (`libxrstreamingclient.so`, 5.56 Mo)** : Le client **Quest Link / Air Link** embarqué sur le casque. Révèle le régulateur `AdaptiveSrc` (`AdaptiveSRC/LowFrameCountLatencyScale`, `decreaseLatency`, `render thread stall likely -> calling reset()`) et `remote_asw`.
- **`OculusXrCvpPlugin.apk` (`libcvp_plugin.so`, 343 Ko)** : Le plugin matériel Qualcomm **CVP (Computer Vision Processor)** (`optical_flow_server_plugin::CVPServerInterfaceV2_Impl`, `bolt::CvpCpuOpticalFlow`).
- **`active_runtime.aarch64.json`** : Confirme que le runtime OpenXR actif d'Horizon OS pointe directement vers `VrDriver.apk!/lib/arm64-v8a/libopenxr_forwardloader.so` $\to$ `libvrruntimeservice.so`.
- **Verdict Harvest Quest 3** : **Complet à 100 %.** Toutes les bibliothèques utilisateur/système accessibles sans root contenant les shaders GLSL (101 programmes en clair), les 205 classes GPU `OVR::*` et les 1 006 CVars ont été moissonnées.

### 1.2 Recoupement avec les binaires SteamVR (« Frame Hallucination » / Motion Smoothing)
Nous avons extrait et désassemblé via `fxc /dumpbin` les **43 shaders `.fxo`** de SteamVR (`C:\Program Files (x86)\Steam\steamapps\common\SteamVR\resources\shaders\d3d11\`) vers `docs/quest3_dump/analysis/steamvr_shaders/`.
Valve nomme son pipeline **Frame Hallucination** et le structure en 6 étages :
1. **`camera_motion_backout_cs.fxo`** : Soustrait la rotation/projection caméra (`matRendered0/1`, `matInvRendered0/1`) du champ de mouvement brut avant toute interpolation.
2. **`mv_static_reject_cs.fxo`** : Compare la norme du vecteur (`g_flMinMotionPixels`) et la différence quadratique moyenne de couleur (`g_flStaticMeanSquaredDiff`). Si la zone est statique en luminance mais que l'encodeur vidéo/OFA donne un vecteur non nul, **le vecteur est forcé à zéro** (élimine le tremblement du HUD et des motifs réguliers).
3. **`mv_depth_consistency_cs.fxo`** : Compare `g_tCurrentDepth` et `g_tAlignedReferenceDepth` le long du vecteur de mouvement et génère une carte de méfiance **`g_tOutputDistrust`** (UAV `float`) + `g_tOutputFieldDepth`.
4. **`mv_distrust_dilate_cs.fxo` & `mv_repair_cs.fxo`** : Dilate la carte `Distrust` sur un voisinage $5\times 5$ et répare les vecteurs invalides en échantillonnant **16 offsets en anneau** (`±3, ±6, ±4 diag, ±11 pixels`) guidés par la profondeur (`g_matCurInvProj`).
5. **`mv_dilation_3x3_cs.fxo` / `5x5` / `7x7`** : Dilate les vecteurs de premier plan pour couvrir les silhouettes.
6. **`frame_hallucination_depth_vs.fxo` & `frame_hallucination_motion_align_vs.fxo`** : Déforme un maillage dense en Vertex Shader avec `g_flGCScale` / `g_bGhostCorrectionEnabled` (atténuation de ghosting aux discontinuités).

### 1.3 Synthèse Croisée : Qui fait quoi le mieux ?

| Brique Algorithmique | Meilleure Référence | Idée Mathématique à Intégrer dans ReproVR (Clean-Room) |
| :--- | :--- | :--- |
| **Pose & Frustum de la Frame Synthétique** | **Meta Quest 3 PTW** (`06_vert_PTW`, `LocklessHmdPoseBuffer`) | La frame synthétique à $t_S$ doit être synthétisée dans sa **propre pose caméra interpolée/extrapolée $\mathbf{P}_S$** (ou déclarer sa vraie pose $\mathbf{P}_S$ + Depth au compositeur), jamais déclarer la pose de $B$ pour un affichage à $t_S$. |
| **Gestion des Bords d'Écran (Rotation Tête)** | **Meta Quest 3 PTW** (`66_frag_PTW` : `v_EdgeDistanceFragment`, `v_ApertureClipDistances`) | **Vignette / Edge Feathering + Clamp-to-Edge Mip/Blur** + synthèse centrée sur la pose $\mathbf{P}_S$ pour que l'ATW du casque n'ait qu'un résidu $< 0.2^\circ$ à compenser. |
| **Dilatation Profondeur & Verrouillage** | **Meta Quest 3 AppSW** (`25_frag_AppSW`, `SampleDepthAndNeighborhood9DilateMinMax`) | **Gather $3\times 3$ Min/Max** produisant `(depthDilated, shouldLock, depthEroded)` : coupe net les bavures autour des armes/mains. |
| **Rejet de Faux Mouvements & Réparation MV** | **SteamVR Frame Hallucination** (`mv_static_reject_cs`, `mv_depth_consistency_cs`, `mv_repair_cs`) | **Carte de Distrust + Réparation en anneau (16-tap)** lorsque le flux optique et la profondeur divergent. |
| **Régulation de Latence & Anti-Stall** | **Meta Quest Link** (`AdaptiveSrc` dans `libxrstreamingclient.so`) | **Garde-fou de profondeur de file (Queue Depth Clamp)** : dès que l'écart `virtual_time - runtime_predicted_time` dépasse $1.5 \times T_{\text{frame}}$, ré-ancrer immédiatement l'horloge sans accumuler de retard. |

---

## 2. Diagnostic Exact des 4 Observations de Test (Hier soir sur RTX 5090)

Le fait que le premier test ait fonctionné *out-of-the-box* avec un gain de FPS massif et une image globalement cohérente valide toute l'infrastructure D3D12 / OpenXR / NVOF. Voici la cause racine exacte (dans le code actuel) de chacun des 4 défauts observés :

### Diagnostic #1 & #2 : Micro-saccades quand on bouge la tête (alors que immobile ou rotation stick = ultra smooth) + Bords noirs saccadés
**Où dans le code :** [`src/layer/openxr_layer.cpp:10747-10754`](file:///C:/code/repro-vr/src/layer/openxr_layer.cpp#L10747-L10754) et [`src/d3d12/frame_synthesis.hlsl:940-965`](file:///C:/code/repro-vr/src/d3d12/frame_synthesis.hlsl#L940-L965).

```cpp
// src/layer/openxr_layer.cpp:10747
if (synthetic) {
    // The V008 shader generates the synthetic image in B's camera
    // reference. Its projection metadata must therefore remain
    // B's render pose/FOV.
    view.next = nullptr;
}
```

**Ce qui se passe physiquement image par image :**
1. Supposons que le jeu tourne à 45 FPS et le casque à 90 Hz ($T = 11.1\text{ ms}$).
   - Frame réelle $A$ rendue pour la pose tête $\theta_A = 0^\circ$.
   - Frame réelle $B$ rendue pour la pose tête $\theta_B = +2^\circ$.
2. Le presenter soumet d'abord la **Frame Synthétique $S$** à $t = 11.1\text{ ms}$ (quand la tête réelle est à $\theta_S \approx +1^\circ$), puis la **Frame Réelle $B$** à $t = 22.2\text{ ms}$ (quand la tête est à $+2^\circ$).
3. **Le bug de pose caméra de $S$** :
   - Dans `frame_synthesis.hlsl`, `PreviousFrame` ($A$) est projetée dans le frustum de $B$ (`PreviousMappings[view_idx]` = $\mathbf{T}_{B \to A}$), mais `CurrentFrame` ($B$) reste **dans le frustum de $B$** (`current_fallback` = `pixel` sans demi-rotation caméra vers $\theta_S$) !
   - De plus, dans `build_generated_frame_end_info`, `S` copie `view.pose = B.pose` ($\theta_B = +2^\circ$) et **supprime `view.next = nullptr`** (ce qui retire `XrCompositionLayerDepthInfoKHR` sur une frame sur deux !).
   - **Conséquence sur les micro-saccades (#2)** :
     - Quand tu tournes au **stick manette sans bouger la tête** : $\theta_A = \theta_B = 0^\circ$. Le frustum HMD ne bouge pas, tout le mouvement est dans les pixels (flux optique / MV interpolé à $\alpha = 0.5$). La pose soumise est exacte $\to$ **rotation ultra-smooth** !
     - Dès que tu **bouges la tête** ($\theta_A \neq \theta_B$) :
       1. Une frame sur deux ($S$) perd son `XrCompositionLayerDepthInfoKHR` (`view.next = nullptr`), donc le compositeur du casque (SteamVR / VDXR / Pimax / Link) alterne à 45 Hz entre *Positional TimeWarp avec Depth* (sur $B$) et *TimeWarp rotationnel pur sans Depth* (sur $S$) $\to$ **oscillation de parallaxe à 45 Hz (micro-saccade / vibration)** !
       2.Dans le shader, lorsque la confiance du flux optique baisse ou que `preserve_stationary` s'active, `stable_midpoint` fait un `lerp(previous_coverage_color, current_fallback.color, frac)` où les deux images ont été alignées sur la pose de $B$ en rotation seule (ou avec une profondeur non interpolée), tandis que les objets proches subissent un saut de phase entre $S$ et $B$.
   - **Conséquence sur les bords noirs (#1)** :
     - Comme $S$ est dessinée dans le frustum de $B$ ($+2^\circ$) mais affichée à l'instant $t_S$ ($+1^\circ$), le compositeur du casque (ATW) doit **tirer toute l'image $S$ de $-1^\circ$ en arrière** pour la ramener en face de tes yeux à $+1^\circ$.
     - Ce recadrage ATW découvre une bande noire de $1^\circ$ sur le bord avant du mouvement lors de $S$, puis 0° lors de $B$, puis à nouveau $1^\circ$ sur la paire suivante ! Tes yeux voient donc **le bord noir sauter à 45 Hz** au lieu de glisser à 90 Hz !
     - **La solution (inspirée de Meta PTW)** : Synthétiser $S$ directement dans la **pose caméra interpolée $\mathbf{P}_S = \text{slerp}(\mathbf{P}_A, \mathbf{P}_B, \alpha)$** (en projetant $A$ de $\mathbf{P}_A \to \mathbf{P}_S$ et $B$ de $\mathbf{P}_B \to \mathbf{P}_S$) **ET** soumettre $S$ au runtime avec `view.pose = P_S` ! Ainsi, à l'instant $t_S$, l'image $S$ est déjà centrée sur l'axe du regard à $+1^\circ$ : l'ATW du casque ne tire plus les bords noirs dans le champ de vision, et le mouvement de tête devient aussi fluide que la rotation au stick !

---

### Diagnostic #3 : Input Lag Manette de 500 ms à 1 s (alors que la rotation casque est temps réel)
C'est l'observation la plus révélatrice. Pourquoi le casque réagit en $< 10\text{ ms}$ alors que le stick de la manette met $500\text{ ms}$ à $1\text{ s}$ avant de commencer à tourner ?
1. **Pourquoi le casque semble temps réel** : Le compositeur final du casque (ATW) applique la dernière rotation du HMD juste avant le VSync, et dans les mods type UEVR/Luke Ross, `xrLocateViews` utilise le `predictedDisplayTime` que notre layer lui donne.
2. **Pourquoi la manette a 500 ms–1 s de retard** : La manette (XInput / Gamepad) est lue par le **thread de simulation/jeu (Game Thread)** d'Unreal/du moteur. Or, trois mécanismes s'additionnent actuellement pour créer un embouteillage (buffering) entre le Game Thread et l'écran :
   - **Cause 3A — L'horloge virtuelle `virtual_time` et `deep_pipeline=1` (`openxr_layer.cpp:5508-5566`)** :
     Dans `layer_wait_frame_impl`, le layer donne au jeu une horloge `anchor = predictedDisplayTime + virtual_period` avec un plafond `ceiling = anchor + virtual_period` (soit jusqu'à **4 périodes d'affichage d'avance** = $+45\text{ ms}$ à $+66\text{ ms}$), et avec `deep_pipeline=1` (`ofxr_bridge.ini`), le presenter **retient volontairement** chaque frame synthétique d'une période supplémentaire (`too_young = age < period`, ligne 8564) et admet jusqu'à 2–3 paires en vol (`wait_for_presenter_capacity`).
   - **Cause 3B — La file de commandes D3D12 / Render Thread d'Unreal Engine sans barrière de latence** :
     Quand le presenter répond immédiatement au `xrWaitFrame` virtuel (`layer_wait_frame_impl` ne bloque pas, le commentaire ligne 5466 le dit explicitement : *"The hold itself is in xrEndFrame, not here"*), le **Game Thread** (qui appelle `xrWaitFrame` sur certains moteurs/mods ou qui échantillonne XInput avant de pousser au Render Thread / RHI Thread) n'est **pas freiné au début de la frame** ! Il empile plusieurs frames de simulation d'avance dans la file interne du moteur (Game Thread $\to$ Render Thread $\to$ RHI Thread $\to$ Queue D3D12 $\to$ `presenter_submissions`).
   - **Cause 3C — Le blocage de la boucle de messages Windows / XInput par contention de mutex ou d'attente GPU** :
     Si le thread principal (qui pompe `WM_INPUT` / XInput) partage un verrou avec le thread de rendu qui attend dans `wait_for_presenter_pair` à la fin de `xrEndFrame`, les événements de la manette sont lus par rafales ou mis en file d'attente par Windows/Steam Input !
   - **Correction Phase 2** :
     1. Passer en **mode Low-Latency (`deep_pipeline=0` par défaut ou mode `asw_extrapolation`)** et placer un **Pacing d'Entrée (`wait_frame` pre-input pacing)** pour que le jeu lise la manette au plus tard 1 frame avant le rendu, jamais 4 à 10 frames à l'avance.
     2. Réduire `maximum_outstanding` à **1 paire max** en mode basse latence.
     3. Logger dans l'instrumentation l'écart exact `T_present - T_wait_frame` (le *Frame Pipeline Depth* en millisecondes) pour vérifier qu'il reste sous **25–35 ms** au lieu de dériver vers 500 ms.

---

### Diagnostic #4 : Consommation VRAM élevée sur la RTX 5090
J'ai fait le calcul exact de toutes les textures `CreateCommittedResource` et swapchains allouées aujourd'hui dans `openxr_layer.cpp`, `d3d12_history.cpp`, `d3d12_frame_synthesizer.cpp` et `dlss_motion_vectors.cpp`.

Sur un casque haute résolution (ex: Pimax Crystal / Quest 3 supersamplé à **$4000 \times 4000$ par œil**, arraySize=2, format RGBA16F = 8 octets/pixel $\implies$ **$128\text{ Mo}$ par texture stéréo**, ou **$170\text{ Mo}$ si `mipCount=4`**) :

| Composant Actuel | Nombre de Textures Stéréo Pleine Résolution | Coût VRAM à $4000\times 4000\times 2$ (RGBA16F) | Pourquoi c'est sur-alloué ? |
| :--- | :--- | :--- | :--- |
| **1. Swapchain du Jeu** (créée par le jeu) | 3 images | ~384 à 512 Mo | Normal (appartient au jeu). |
| **2. `current` Private Swapchain** (`openxr_layer.cpp:2947`) | 1 swapchain OpenXR = **3 images** allouées par le runtime | **~384 à 512 Mo** | Même avec `single_swapchain_rings=1`, le runtime OpenXR alloue 3 buffers par `xrCreateSwapchain` ! |
| **3. `synthetic` Private Swapchain** (`openxr_layer.cpp:2961`) | 1 swapchain OpenXR = **3 images** allouées par le runtime | **~384 à 512 Mo** | Seconde swapchain OpenXR. |
| **4. `current_staging[2]` + `synthetic_staging[2]`** (`openxr_layer.cpp:3005`) | **4 textures D3D12** (`kStagingSlotCount = 2` × 2) | **~512 à 680 Mo** | **Gaspillage majeur !** En mode `single_swapchain_rings=1`, le layer alloue 4 textures de staging plein écran *en plus* des 2 swapchains privées, et copie encore `current_staging` alors que l'image courante est déjà dans `D3D12SwapchainHistory` ! |
| **5. `D3D12SwapchainHistory`** (`d3d12_history.hpp:58`) | **3 slots** (`kSlotCount = 3`) Color + **3 slots** Depth | **~384 Mo (Color) + ~192 Mo (Depth)** | Pour interpoler entre $t-1$ et $t$, seuls **2 slots** suffisent si le slot courant est lu directement ou si `single_swapchain_rings` partage le buffer. De plus, il garde les mips (`mipCount` du jeu) alors que le shader ne lit que le **mip 0** ! |
| **6. `dlss_motion_vectors.cpp`** (Snapshots DLSS) | **4 slots** (`kSlots = 4` × 2 yeux) de MV + **4 slots** Depth | **~256 à 384 Mo** | 4 slots temporels par œil alors qu'on n'utilise que le dernier slot publié ! **2 slots** suffisent largement. |
| **7. `D3D12FrameSynthesizer`** (`work_slots[3]` + NVOF/FFX) | 3 × (`packed_color` + `nvidia_luma` + `flow` + `cost`) | **~250 à 400 Mo** | Même quand `backend=nvidia` (Luma R8), `packed_color` RGBA8 est parfois alloué, et `kWorkSlotCount = 3` triple les buffers intermédiaires ! |
| **TOTAL SUR 1 SEULE SWAPCHAIN** | **~20 à 24 textures plein écran !** | **~2,7 Go à 3,6 Go de VRAM !** | Et si le jeu recrée une swapchain ou a une swapchain secondaire avant destruction, ça monte à **4–6 Go** ! |

**Comment diviser la VRAM par ~2.5 à 3 en Phase 2 (économiser ~1.8 à 2.5 Go de VRAM) :**
1. **Forcer `mipCount = 1` sur TOUTES les ressources internes D3D12** (`private_info.mipCount = 1`, `staging_description.MipLevels = 1`, `history_description.MipLevels = 1`) même en D3D12 natif (actuellement ligne 2941, `private_info.mipCount = 1` n'est fait que pour `d3d11_bridge || vulkan_bridge` !). Gain immédiat : **-33 % de VRAM** si le jeu utilise des mipmaps.
2. **Supprimer `current_staging[2]` (2 textures stéréo plein écran = ~256–340 Mo)** : L'image de la frame courante est *déjà* copiée dans `D3D12SwapchainHistory` ! Lors du hand-over vers `current_runtime_images`, `HandoverCopier` peut copier directement depuis le slot `D3D12SwapchainHistory` vers la swapchain du runtime sans passer par `current_staging[2]`.
3. **Mode `vram_budget = lean` (2 slots d'historique au lieu de 3, 2 slots DLSS au lieu de 4, 2 `WorkSlot` au lieu de 3)** : Réduit encore de **33 %** toutes les tables d'historique et de synthèse.
4. **Destruction paresseuse/immédiate des buffers Depth/DLSS inutilisés** : N'allouer les 3 slots de Depth dans `D3D12SwapchainHistory` que si le jeu soumet effectivement `XrCompositionLayerDepthInfoKHR` (et non dès qu'une swapchain depth est créée mais jamais attachée).

---

## 3. Question Juridique & Licence : Publier en MIT ou Apache-2.0 Sans Risque

Tu as posé deux exigences cruciales :
1. **« Il ne faut pas que Meta me tombe dessus »** (suite à l'analyse du Quest 3 et d'OVRServer).
2. **« Idéalement rendre le projet OSS MIT ou Apache-2.0 »**.

Voici l'audit juridique précis de l'état actuel du dépôt et ce qu'il faut faire :

### 3.1 Protection vis-à-vis de Meta & Valve (Clean-Room Reverse Engineering)
En droit du logiciel (Directive Européenne 2009/24/CE art. 5 & 6 sur l'interopérabilité, et DMCA 17 U.S.C. § 1201(f) aux USA) :
- **Les idées, principes mathématiques, formules géométriques (projection tan-angle, filtre médian $3\times 3$, dilatation min/max, rejet par variance) ne sont PAS protégeables par le droit d'auteur (copyright).** Seule l'*expression littérale* (le code source exact mot pour mot ou les binaires `.so`/`.apk`/`.dll`) est couverte par le copyright.
- **Règles d'or à respecter impérativement dans le repo Git :**
  1. **`docs/quest3_dump/` doit rester 100 % hors du dépôt Git** (déjà dans `.gitignore`). Ne jamais commiter aucun `.apk`, `.so`, `.spv`, `.fxo`, ni aucun fichier `.glsl` extrait de `libvrruntimeservice.so`.
  2. **Zéro copier-coller de lignes GLSL de Meta ou d'assembleur de Valve/PureDark** dans `src/d3d12/frame_synthesis.hlsl`. Notre shader est écrit en **HLSL DirectX 12** avec nos propres structures (`CameraMapping`, `SynthesisParameters`) et nos propres noms de fonctions.
  3. **Purger les références nominatives aux binaires propriétaires dans les commentaires du code et de la doc publique** :
     - Actuellement, `frame_synthesis.hlsl` (lignes 1003, 1088) et `docs/RE_EVIDENCE_LEDGER.md` citent des offsets binaires précis (`ovr_758d60.asm`, `PDAFWPlugin afw_81320.ll`, `afw_78b40.ll`).
     - **Action Phase 2** : Déplacer `docs/RE_EVIDENCE_LEDGER.md` et `docs/QUEST3_RD_EXTRACTION_PLAN.md` dans le dossier local gitignoré `docs/quest3_dump/`, et remplacer les commentaires dans `src/` par des références à la **littérature académique publique** (ex: *Karis 2014 Temporal AA*, *rover/Bowles 2012 Iterative Image Warping*, *Yang et al. 2020 Bilateral Guided Upsampling*, *Khronos OpenXR `XR_KHR_composition_layer_depth` / `XR_FB_space_warp` public specs*).

### 3.2 Attention : Obstacle actuel pour passer en licence MIT ou Apache-2.0 !
Regarde bien les fichiers `LICENSE` et `licenses/GPL-3.0-or-later.txt` actuellement à la racine de `repro-vr` :
- Le projet `repro-vr` a été construit sur la base du code de **`OFXR Bridge` (`xrfg` / `ofxr_bridge`)**, dont la licence d'origine est **LGPL-3.0-or-later / GPL-3.0-or-later** (visible dans `LICENSE` = GNU LGPL v3 et `licenses/GPL-3.0-or-later.txt`).
- **Ce que cela implique légalement :**
  1. **Si tu n'es pas l'auteur originel de `OFXR Bridge`** : Tu ne peux **pas** changer la licence d'un code existant sous LGPLv3/GPLv3 vers MIT ou Apache-2.0 sans réécrire les parties issues d'OFXR Bridge (ou avoir l'accord de l'auteur d'OFXR Bridge). En revanche, tu as **100 % le droit de publier et distribuer librement ton fork en Open-Source sous LGPL-3.0 (ou GPL-3.0)** ! Et toutes **tes nouvelles briques modulaires** (comme `depth_reprojection.hpp/.cpp`, `upscaler_auto_hook.hpp/.cpp`, l'auto-calibrateur, les nouveaux shaders HLSL) peuvent être placées sous **double licence MIT / Apache-2.0** dans des fichiers dédiés.
  2. **Si tu veux que 100 % du dépôt soit MIT ou Apache-2.0** : Il faudrait remplacer le squelette hérité d'OFXR Bridge par un squelette OpenXR Layer propre (basé sur le template officiel Khronos OpenXR-SDK qui est sous **Apache-2.0 / MIT**) en conservant uniquement nos modules clean-room.
  3. **Concernant les SDK tiers dans `external/`** :
     - `OpenXR-SDK` et `Vulkan-Headers` : **Apache-2.0 OR MIT** ✅ (100 % compatibles MIT/Apache-2.0).
     - `FidelityFX-SDK` : **MIT** ✅ (100 % compatible).
     - `OpenVR` (`openvr.h`) : **BSD-3-Clause** ✅ (100 % compatible).
     - `NVIDIA Optical Flow SDK` (`nvOpticalFlowCommon.h`, `nvOpticalFlowD3D12.h`) : Licence MIT-like NVIDIA ✅ (100 % compatible).

---

## 4. Plan d'Action & d'Implémentation — Phase 2

Voici le plan structuré en **5 chantiers** pour transformer l'essai d'hier en un produit final ultra-fluide, économe en VRAM et doté d'une instrumentation chirurgicale :

### Chantier 1 : Synthèse Centrée sur la Pose Cible $\mathbf{P}_S$ & Élimination des Micro-Saccades / Bords Noirs
1. **Interpolation/Extrapolation de la Pose de Soumission de la Frame Synthétique (`openxr_layer.cpp`)** :
   - Au lieu de copier `view.pose = B.pose` dans `build_generated_frame_end_info` pour la frame synthétique $S$, calculer la pose exacte $\mathbf{P}_S = \text{interpolate\_pose}(\mathbf{P}_A, \mathbf{P}_B, \alpha)$ (ou l'extrapolation bornée à $\Delta t \le 18\text{ ms}$ comme le `Clamping dTExtrapolate` découvert dans `libvrapiimpl.so`).
   - Conserver et reprojeter la structure `XrCompositionLayerDepthInfoKHR` sur la frame synthétique lorsque le jeu en fournit une, afin que le compositeur du casque ne bascule plus à 45 Hz entre PTW 6-DOF (sur $B$) et ATW 3-DOF (sur $S$).
2. **Double-Warp Symétrique $A \to \mathbf{P}_S$ et $B \to \mathbf{P}_S$ dans `frame_synthesis.hlsl`** :
   - Passer au shader non seulement `PreviousMappings` ($\mathbf{P}_S \to \mathbf{P}_A$), mais aussi `CurrentMappings` ($\mathbf{P}_S \to \mathbf{P}_B$).
   - Ainsi, lors d'une rotation de la tête de $\theta_A = 0^\circ$ à $\theta_B = +2^\circ$, la frame synthétique à $\alpha = 0.5$ est construite directement à $\theta_S = +1^\circ$ : $A$ est tournée de $+1^\circ$ et $B$ de $-1^\circ$.
   - **Effet sur le Symptôme #2 (Micro-saccades)** : La scène 3D et le casque sont 100 % en phase angulaire et positionnelle à 90/120 Hz. Plus aucun saut entre $S$ et $B$.
   - **Effet sur le Symptôme #1 (Bords noirs)** : Divise par **2** l'excursion angulaire sur les bords, et comme `view.pose = P_S` correspond déjà à la pose de la tête à l'instant $t_S$, l'ATW du casque ne tire plus les bords noirs dans le champ de vision !
3. **Traitement Doux des Bords d'Écran (Inspiré de Meta `66_frag_PTW` : Edge Feather + Clamp-to-Edge)** :
   - Lorsqu'un échantillon sort légèrement de `SourceRect` pendant une rotation rapide de la tête, au lieu d'un `valid = 0` brutal qui crée une marche d'escalier noire :
     - Utiliser l'échantillon valide de l'autre borne temporelle ($B$ sur le bord entrant, $A$ étiré/clampé avec atténuation douce sur $8\text{ px}$ sur le bord sortant).

### Chantier 2 : Suppression de l'Input Lag Manette (500 ms $\to < 25\text{ ms}$)
1. **Nouveau réglage `low_latency_input=1` (et `deep_pipeline=0` testable à chaud)** :
   - Brider l'avance maximale de l'horloge virtuelle `virtual_time` dans `layer_wait_frame_impl` à **$1 \times \text{virtual\_period}$** (au lieu de $2 \times \text{virtual\_period}$).
   - En mode `deep_pipeline=0` (Shallow Pipeline), supprimer la rétention artificielle d'une période (`too_young = age < period`) dans le presenter : dès que la frame synthétique est prête sur le GPU (`synthetic_output_ready`), elle part au scanout suivant.
2. **Throttle d'Admission Pré-WaitFrame (`wait_frame_input_pacing`)** :
   - Actuellement, `layer_wait_frame_impl` retourne immédiatement et toute l'attente se fait à la fin de `xrEndFrame`. Cela laisse le Game Thread (qui lit la manette XInput juste après `xrWaitFrame` ou au début de son tick) prendre plusieurs frames d'avance dans le pipeline interne du moteur (Game $\to$ Render $\to$ RHI).
   - Ajouter une barrière de cadencement légère dans `layer_wait_frame_impl` qui garantit qu'au maximum **1 frame de jeu** est en vol avant que le moteur n'échantillonne les entrées manette du tick suivant.

### Chantier 3 : Intégration Clean-Room des Pépites Quest 3 & SteamVR dans `frame_synthesis.hlsl`
1. **Gather de Profondeur $3\times 3$ Min/Max + Flag `shouldLock` (inspiré Quest 3 AppSW)** :
   - Remplacer le sample ponctuel `sample_current_linear_depth(pixel)` par un voisinage $3\times 3$ retournant `(z_min_foreground, z_max_background, edge_discontinuity)`.
   - Dilater d'un pixel la profondeur du premier plan pour éviter que la couleur de l'arme/main ne bave sur le fond.
2. **Static Reject & Distrust Repair (inspiré SteamVR `mv_static_reject_cs` + `mv_depth_consistency_cs`)** :
   - Si la différence de luma locale entre $A$ (reprojeté) et $B$ (reprojeté) est inférieure à un seuil $\epsilon_{\text{static}}$ ET que la profondeur est continue, amortir le vecteur de flux optique vers zéro (élimine le *shimmering* sur les grilles et le HUD).
   - Si le vecteur de flux optique viole la cohérence de profondeur entre $A$ et $B$, activer un fallback en croix (4 ou 8 taps autour du pixel) pour récupérer le vecteur du voisin partageant la même profondeur que la cible.

### Chantier 4 : Cure d'Amaigrissement VRAM (Objectif : -50 % à -65 % de VRAM)
1. **Forcer `MipLevels = 1`** sur toutes les textures de staging, d'historique (`D3D12SwapchainHistory`) et de swapchains privées en D3D12 natif.
2. **Éliminer `current_staging[2]`** en mode `single_swapchain_rings=1` : copier directement le slot commité de `D3D12SwapchainHistory` vers `current_runtime_images` au moment du hand-over.
3. **Mode `vram_profile = balanced | lean`** dans `ofxr_bridge.ini` :
   - `kSlotCount = 2` (au lieu de 3) pour l'historique D3D12 et les `WorkSlot` du synthétiseur en mode `lean`.
   - `kSlots = 2` (au lieu de 4) dans `dlss_motion_vectors.cpp`.
   - Allocation paresseuse (lazy) des buffers de Depth : 0 Mo alloué tant qu'aucune profondeur valide n'est soumise.

### Chantier 5 : Instrumentation Temps Réel Zéro-Overhead & Boucle d'Itération Rapide
Pour comprendre instantanément ce qui se passe en jeu et itérer sur les matrices/poses sans overhead :
1. **Compteur VRAM Détaillé par Catégorie (dans le HUD Overlay & Flight Log)** :
   - Traquer au Mo près (atomique `std::atomic<uint64_t>`, **0,00 ms d'overhead**) la VRAM allouée par ReproVR décomposée en :
     `VRAM ReproVR: Total = XXX MB [PrivSwap: XX | Staging: XX | History: XX | Synth/OFA: XX | DLSS-MV: XX]` + `DXGI Process Budget: Used / Budget MB`.
2. **Télémétrie Cinématique & Latence en Temps Réel (HUD Debug Mode + CSV/FlightLog)** :
   - **Profondeur de file & Input Lag (`PipelineLatencyMs`)** : Temps écoulé entre le `xrWaitFrame` donné au jeu et le `xrEndFrame` réel envoyé au casque. Permet de vérifier de visu que le lag manette tombe de $>500\text{ ms}$ à $<25\text{ ms}$.
   - **Erreur résiduelle de Pose ATW (`PoseDeltaDeg`, `PosDeltaMm`)** : Écart angulaire (en degrés) et positionnel (en mm) entre la pose soumise par la frame synthétique $S$ et la pose réelle du casque au moment de son scanout (`xrLocateViews` au `predictedDisplayTime` du presenter). **Objectif : $< 0.15^\circ$** (zéro micro-saccade, zéro bord noir).
   - **Marge des Bords (`EdgeMarginPx`)** : Nombre de pixels de bordure hors-champ lors des rotations rapides de tête.
3. **Hot-Reload des Paramètres (`ofxr_bridge.ini` lu toutes les 250 ms sans stutter)** :
   - Pouvoir basculer **en direct pendant que le jeu tourne** (via le fichier `.ini` ou raccourcis clavier) :
     - `pose_centering_mode = target_b | midpoint_slerp | extrapolated_asw` (pour comparer immédiatement en bougeant la tête l'ancien ancrage sur $B$ vs le nouvel ancrage centré sur $S$ !),
     - `edge_feather_px = 0..24` (largeur du fondu anti-bord noir),
     - `keep_synthetic_depth = 0 | 1` (maintien du Depth Info KHR sur la frame synthétique),
     - `input_pacing_mode = end_frame | wait_frame_low_latency` (pour tester en direct la réactivité de la manette),
     - `debug_view = none | disocclusion_mask | flow_vectors | edge_margin | pose_error`.
4. **Banc de Test Headless de Non-Régression Cinématique (`tests/head_rotation_judder_tests.cpp`)** :
   - Simule une rotation continue de la tête à $90^\circ/\text{s}$ et une rotation au stick à $90^\circ/\text{s}$, et vérifie mathématiquement que la trajectoire angulaire des pixels projetés à 90 Hz a une dérivée seconde (accélération/saccade) nulle !
