# Registre de Preuves Rétro-Ingénierie (RE Evidence Ledger)

Ce document consigne les preuves techniques exactes extraites lors de l'analyse binaire (méthodologie *Evidence-First* inspirée de `morluto/rea`) des runtimes de référence présents sur la machine hôte :
1. **Meta Horizon PC Runtime** (`OVRServer_x64.exe` & `LibOVRRTImpl64_1.dll`) — *ASW 2.0, AppSW, Positional TimeWarp (PTW) et NVOF Optical Flow*.
2. **PureDark Action Frame Warp** (`PDAFWPlugin.dll` & `PDAFWPlugin.h`) — *Reconstruction Stéréo-Temporelle AER/AFW pour UEVR*.

---

## 1. Meta Horizon PC (`OVRServer_x64.exe`) — ASW 2.0 & Positional TimeWarp

### 1.1 Classes RTTI & Architecture Interne Identifiées
L'inspection de la table RTTI et des chaînes de télémétrie de `OVRServer_x64.exe` confirme la séparation entre l'estimateur de flux optique matériel et le pipeline de synthèse par Compute Shaders :
- `ASWExtrapolator` : Orchestrateur principal d'extrapolation temporelle.
- `NVOFFrameEstimator` : Wrapper autour de l'API NVIDIA Optical Flow (`NvOFInterface`), alimentant `ASWExtrapolator` en vecteurs de mouvement par blocs $4\times 4$.
- `ASWCSStageProcessor` : Exécuteur séquentiel des passes Compute Shader DX11/DX12 d'inversion de mouvement et de tri d'occlusions.
- `ExtrapolatorForStereoReconstruction` : Module de transfert inter-oculaire utilisé pour la cohérence binoculaire.
- `PositionalTimeWarp` (`PTW`) : Pipeline de déformation 6-DOF par grille de profondeur dans le compositeur.

### 1.2 Variables de Configuration Internes (`asw.*` & `oculus_pasw`)
Les commandes de diagnostic internes extraites du binaire révèlent les leviers exacts du pipeline Meta :
- `asw.Depth` : Active l'utilisation du Depth Buffer de l'application (ASW 2.0 / PTW) pour guider l'inversion des vecteurs et la parallaxe 6-DOF.
- `asw.Median` : Active le filtre médian vectoriel $3\times 3$ adaptatif avant l'extrapolation.
- `asw.Adaptive` : Bascule dynamiquement entre l'extrapolation ASW guidée par profondeur et le repli TimeWarp 3-DOF lorsque la divergence locale dépasse le seuil de confiance.
- `oculus_pasw` : Active *Passthrough / AppSW*, où le moteur fournit directement ses vecteurs de mouvement Ground-Truth.

### 1.3 Pipeline Compute Shader ASW 2.0 (5 Passes DXBC)
L'extraction et le désassemblage des 44 shaders DXBC embarqués dans `OVRServer_x64.exe` mettent en évidence un pipeline à 5 étages pour transformer un champ de mouvement *Forward* ($t-1 \to t$) en un champ *Backward* ($t+\alpha \to t$) trié par profondeur :

| Passe | Offset Binaire | Modèle | Rôle Technique & Instructions Clés |
| :--- | :--- | :--- | :--- |
| **Pass 1 : `QuadSetupCS`** | `0x755690` (`ovr_755690.asm`) | `cs_5_0` `[8,8,1]` | Déplace les sommets des macroblocs selon `-MV * poseExtrapolation * mvToPixelScale / mbPixelSize`. Calcule une **pseudo-profondeur cinématique** de secours : `dp2 r1.z, r0.xyxx, r0.xyxx` ($\text{depth} = \|\mathbf{MV}\|^2$) afin que les objets rapides (premier plan) recouvrent le fond statique même sans Depth Buffer. |
| **Pass 2 : `QuadTreeSeedCS`** | `0x755e50` (`ovr_755e50.asm`) | `cs_5_0` `[8,8,1]` | Calcule les boîtes englobantes 2D (`min/max` AABB) de chaque quad déformé et initialise la base de la pyramide (`outputExtents`). |
| **Pass 3 : `QuadTreeBuildCS`** | `0x754c70` (`ovr_754c70.asm`) | `cs_5_0` `[8,8,1]` | Construit une hiérarchie spatiale (BVH Quadtree à **12 niveaux**, `outputExtents[12]`) par réduction successive des `min` et `max` AABB. |
| **Pass 4 : `QuadTreeTraverseCS`** | `0x758d60` (`ovr_758d60.asm`) | `cs_5_0` `[8,8,1]` | Parcourt la pile BVH (`x0`), collecte jusqu'à **10 quads candidats** (`x1[10]`) recouvrant le pixel cible, effectue un test d'intersection scanline 2D et **retient le candidat de profondeur maximale** (`ge r1.w, r3.w, r1.x`) pour résoudre proprement l'occlusion premier plan / arrière-plan. |
| **Pass 5 : `MedianAndDilateCS`** | `0x7530b0` / `0x753760` | `cs_5_0` `[8,8,1]` | Applique un filtrage médian vectoriel et dilate les vecteurs du premier plan sur les bordures de désocclusion. |

### 1.4 Positional TimeWarp (`ovr_674140.asm`)
Le Vertex/Pixel Shader du compositeur (`0x674140`) implémente le Positional TimeWarp 6-DOF :
- Constantes extraites : `SampleDepthLocation`, `SampleDepthLerpCoeff`, `SampleDepthProjMatrix`, `EyeToSourceUVScale`, `EyeToSourceUVOffset`, `FinalLayer`.
- Mécanisme : Échantillonne le buffer de profondeur à la position projetée, convertit le $Z$ matériel en distance métrique via `SampleDepthProjMatrix`, applique la translation 6-DOF $\Delta \mathbf{T}$ entre la pose de rendu et la pose HMD actuelle, et re-compose les couches d'interface (`FinalLayer`) séparément pour éviter toute distorsion du HUD.

---

## 2. PureDark Action Frame Warp (`PDAFWPlugin.dll`) — Reconstruction Stéréo-Temporelle

### 2.1 Interface Native (`PDAFWPlugin.h`)
L'en-tête et la table d'export de `PDAFWPlugin.dll` exposent trois points d'entrée D3D12 :
- `InitDevice(ID3D12Device*)`
- `InitFrameWarp(...)`
- `EvaluateFrameWarp(ID3D12GraphicsCommandList*, ...)`

Les ressources consommées sont :
- Couleur et Profondeur de l'œil fraîchement rendu,
- Vecteurs de mouvement Unreal Engine (`VelocityTex`, encodés avec `decode_velocity`),
- Matrices complètes `ClipToWorld`, `WorldToClip`, `ClipToView`, `ViewToClip` (courantes et précédentes) pour les deux yeux.

### 2.2 Pipeline DXIL (`sm_6_2` / `sm_6_6`) en 3 Étages
Le désassemblage via `dxc.exe -dumpbin` des 42 shaders DXIL embarqués dans `PDAFWPlugin.dll` révèle l'algorithme exact de synthèse d'un œil (ou d'une frame intermédiaire) sans artefacts de dédoublement :

| Passe | Offset Binaire | Rôle Technique & Opérations atomiques DXIL |
| :--- | :--- | :--- |
| **Pass 1 : `ForwardDepthSplatCS`** | `0x81320` (`afw_81320.ll`) & `0x74470` (`afw_74470.ll`) | Décode la vitesse moteur (`_VelocityTex`), reprojette chaque pixel 3D de la source vers la caméra cible via `_SrcClipToWorld` $\to$ `_DestWorldToClip` (ou `_SrcClipToWorldPrev` si l'objet est en mouvement), et projette la profondeur dans `_DestDepthTex` via un **Z-buffer atomique** (`dx.op.atomicBinOp.i32(..., UMax)` ou `i64` sur `_ReprojectedDataPacked`). Incrémente également un compteur de couverture par pixel (`_CountTex1` / `_CountTex2`) pour détecter les trous de désocclusion (`Count == 0`). |
| **Pass 2 : `DepthTestedGatherWarpCS`** | `0x7bd90` (`afw_7bd90.ll`) | Re-parcourt la grille, teste si la profondeur reprojetée du pixel candidat correspond au vainqueur atomique enregistré dans `_DestDepthTex` (`icmp eq i32 %315, %316`), et n'écrit la couleur dans `_DestColorTex` (ainsi qu'un masque `_MovingObjEyeMaskTex`) que pour la surface la plus proche. |
| **Pass 3 : `DisocclusionHoleFillCS`** | `0x78b40` (`afw_78b40.ll`) & `0x7a7b0` (`afw_7a7b0.ll`) | Pour chaque pixel où `_CountTex == 0` (zone nouvellement découverte derrière un objet en mouvement ou par parallaxe inter-oculaire), résout la couleur en consultant successivement **`_OtherEyeLookUp`** (projection stéréo depuis l'autre œil) puis **`_PreviousFrameLookUp`** (historique temporel), éliminant ainsi les halos typiques de l'interpolation mono-vue. |

---

## 3. Synthèse & Supériorité Architecturale de ReproVR

En combinant les enseignements de ces deux architectures au sein de **ReproVR** :
1. **Tri d'occlusion par profondeur réelle + pseudo-profondeur cinématique** : Nous intégrons le Z-test premier plan / arrière-plan de `PDAFWPlugin` (`afw_81320`/`afw_7bd90`) *et* le repli par pseudo-profondeur cinématique $\|\mathbf{MV}\|^2$ de Meta ASW (`ovr_755690`) lorsque le buffer de profondeur n'est pas disponible.
2. **Parallaxe 6-DOF complète + Décomposition $\mathbf{V}_{\text{cam}}$ vs $\mathbf{V}_{\text{obj}}$** : Contrairement aux approches purement 3-DOF (rotation seule), ReproVR calcule le déplacement 3D exact induit par la translation HMD ($\Delta \mathbf{T}$) et sépare le flux caméra du mouvement propre des objets dynamiques.
3. **Remplissage des désocclusions Stéréo-Temporel (Cross-Eye + Bidirectional)** : Les zones occultées dans une vue temporelle ou stéréo sont reconstruites en exploitant à la fois la vue opposée ($L \leftrightarrow R$) et la paire temporelle ($t-1 \leftrightarrow t$).
4. **Zéro dépendance externe** : Là où PureDark AFW nécessite un plugin UEVR spécifique au moteur Unreal et où Meta AppSW requiert une intégration SDK native, ReproVR intercepte automatiquement `NVSDK_NGX_D3D12_EvaluateFeature` (DLSS/Streamline) en mémoire ou bascule sur `NVOF 5.x` (Blackwell R8 Luma) / `FidelityFX OF`.
