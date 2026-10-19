# Feuille de Route R&D & Rétro-Ingénierie Quest 3 (Horizon OS / XR2 Gen 2) pour ReproVR

Ce document formalise la stratégie d'investigation, d'extraction via **ADB** et de rétro-ingénierie binaire approfondie sur le **Meta Quest 3** (SoC Qualcomm Snapdragon XR2 Gen 2 sous Horizon OS / Android 12-14). 

L'objectif est d'intercepter les dernières avancées propriétaires de Meta (2023–2026) dans le domaine du **SpaceWarp mobile**, de l'**Edge-Aware Reprojection**, du **MQSR (Super Resolution)**, et des algorithmes de gestion des occlusions, afin de transposer ces pépites algorithmiques dans le pipeline HLSL DirectX 12 de **ReproVR** sur PCVR (RTX 5090).

---

## 1. Cibles Binaires Prioritaires à Moissonner via ADB

Horizon OS stocke ses services VR, ses runtimes OpenXR et ses bibliothèques de distorsion / reprojection dans `/vendor`, `/system`, `/apex` et dans les APK système.

### A. Bibliothèques Partagées Systèmes (`.so` ARM64-v8a)
| Cible | Emplacement Typique | Intérêt pour ReproVR |
| :--- | :--- | :--- |
| **`libAppSpaceWarp.so` / `libspacewarp.so`** | `/vendor/lib64/`, `/system/lib64/` | Implémentation officielle d'AppSW (`XR_FB_space_warp`) : calcul des vecteurs, échantillonnage et gestion des trous de désocclusion. |
| **`libovrwarp.so` / `libtimewarp.so`** | `/vendor/lib64/hw/`, `/system/lib64/` | Shaders de TimeWarp asynchrone (ATW / PTW), correction de distorsion optique, chromatique et matrices de projection. |
| **`libopenxr_meta.so` / `libopenxr_runtime.so`** | `/vendor/lib64/`, `/system/lib64/` | Runtime OpenXR propriétaire Meta, gestion des swapchains multi-vues, extensions `XR_FB_*`. |
| **`libmqsr.so` / `libmetasr.so`** | `/vendor/lib64/`, `/system/lib64/` | Moteur de Super Résolution de Meta (MQSR) : reconstruction d'arêtes et upscaling spatial couplé à la reprojection. |
| **`libvulkan_adreno.so`** | `/vendor/lib64/hw/` | Pilote Vulkan Adreno 740 : extraction des pipelines et shaders compilés embarqués. |
| **Bibliothèques DSP / NPU (Hexagon)** | `/vendor/lib64/rfsa/adsp/` | Modèles d'inférence de mouvement ou débruitage neural s'ils tournent sur le NPU du Quest 3. |

### B. Packages et Runtimes Système (APK)
- `/system/priv-app/OvrRuntime/OvrRuntime.apk` (ou `com.oculus.runtime`)
- `/system/app/VrShell/VrShell.apk` (ou `com.oculus.vrshell`)
- `/data/app/~~*/com.oculus.systemdriver-*/` : Pilotes et mises à jour OTA du compositeur VR.

---

## 2. Script de Moisson Automatisée ADB (`harvest_quest3.ps1`)

Script PowerShell à exécuter lors de la session connectée en USB (avec le mode développeur et ADB activés sur le Quest 3) :

```powershell
# harvest_quest3.ps1
$DEST_DIR = "C:\code\repro-vr\docs\quest3_dump"
New-Item -ItemType Directory -Force -Path $DEST_DIR | Out-Null
New-Item -ItemType Directory -Force -Path "$DEST_DIR\libs" | Out-Null
New-Item -ItemType Directory -Force -Path "$DEST_DIR\apks" | Out-Null
New-Item -ItemType Directory -Force -Path "$DEST_DIR\props" | Out-Null

Write-Host "[*] Verification de la connexion ADB Quest 3..." -ForegroundColor Cyan
adb devices -l

Write-Host "[*] Dump des proprietes systeme (build, soc, cvars)..." -ForegroundColor Yellow
adb shell getprop > "$DEST_DIR\props\getprop.txt"
adb shell dumpsys SurfaceFlinger > "$DEST_DIR\props\surfaceflinger.txt" 2>&1

Write-Host "[*] Recherche et extraction des bibliotheques SpaceWarp / TimeWarp / OpenXR..." -ForegroundColor Yellow
$TARGET_LIBS = @(
    "*spacewarp*", "*warp*", "*openxr*", "*mqsr*", "*oculus*", "*asw*", "*timewarp*"
)

foreach ($pattern in $TARGET_LIBS) {
    $found = adb shell "find /vendor/lib64 /system/lib64 /apex -name '$pattern.so' 2>/dev/null"
    foreach ($line in $found) {
        $path = $line.Trim()
        if ($path -and -not $path.Contains("Permission denied")) {
            Write-Host "  -> Extraction de $path" -ForegroundColor Green
            adb pull $path "$DEST_DIR\libs\"
        }
    }
}

Write-Host "[*] Extraction des APKs de runtime VR..." -ForegroundColor Yellow
$packages = adb shell "pm list packages -f | grep -E 'oculus|runtime|vr'"
foreach ($pkg in $packages) {
    if ($pkg -match "package:(.*\.apk)=(.*)") {
        $apkPath = $matches[1]
        $pkgName = $matches[2].Trim()
        Write-Host "  -> Pull APK $pkgName : $apkPath" -ForegroundColor DarkCyan
        adb pull $apkPath "$DEST_DIR\apks\$pkgName.apk"
    }
}

Write-Host "[+] Moisson Quest 3 terminee dans $DEST_DIR !" -ForegroundColor Green
```

---

## 3. Protocole de Rétro-Ingénierie Musclée (Méthodologie REA)

Une fois les binaires extraits, nous déploierons un pipeline d'analyse forensique automatisé en 4 étapes :

### Étape 1 : Cartographie des Symboles, Chaînes et CVars Internes
- Extraction exhaustive des symboles de débogage non stripés (ELF symbol tables, `.dynsym`, `.symtab`) via `llvm-nm` / `readelf`.
- Recherche de motifs textuels (`strings -a`) sur les mots-clés :
  - `warp`, `disocclusion`, `dilation`, `hole`, `confidence`, `specular`, `velocity_scale`, `canting`, `cross_eye`.
- Extraction des variables de configuration cachées de Meta (CVars `debug.oculus.*`, `persist.ovr.*`).

### Étape 2 : Extraction et Décompilation des Shaders Embarqués (SPIR-V)
Les bibliothèques Android Vulkan embarquent leurs shaders sous forme de blobs binaires **SPIR-V** (souvent compressés ou concaténés) :
1. Recherche des magic numbers SPIR-V (`0x07230203` en Little Endian : `03 02 23 07`).
2. Découpage automatique des flux d'instructions SPIR-V via script Python.
3. Décompilation en code lisible via `spirv-cross --glsl` ou `spirv-cross --hlsl`.
4. Analyse comparative directe avec notre shader [`src/d3d12/frame_synthesis.hlsl`](file:///C:/code/repro-vr/src/d3d12/frame_synthesis.hlsl).

### Étape 3 : Décompilation ARM64 (Ghidra / IDA Pro) des Fonctions Mathématiques
Focus sur les fonctions clés de déformation géométrique :
- Calcul de l'extrapolation de pose (gestion des quaternions et des dérivées d'accélération IMU).
- Algorithme de sélection de profondeur avant-plan/arrière-plan (*Foreground / Background Depth Slicing*).
- Formules de pondération temporelle et rejet des discontinuités de vecteurs.

---

## 4. Pépites R&D Précises à Rechercher (Ce qui peut « Booster » ReproVR)

### 1. Dilatation Morphologique de Silhouette (*Edge Dilation & Anti-Bleed*)
* **Problème en reprojection :** Lorsqu'un personnage ou une arme en premier plan se déplace rapidement devant un fond éloigné, les pixels de bordure échantillonnent souvent un mélange de couleur du premier plan et de profondeur de l'arrière-plan, créant un liseré étiré baveux.
* **Ce qu'on cherche dans Quest 3 :** Leur kernel de dilatation morphologique (souvent un filtre en croix à 5 taps ou $3 \times 3$ rapide). Le Quest 3 "gonfle" artificiellement la profondeur des pixels de premier plan d'environ 1 à 2 pixels pour forcer l'occlusion à découper nettement la silhouette.
* **Transposition ReproVR :** Ajouter un mini-pass de dilatation de masque ou l'intégrer dans le gather de [`frame_synthesis.hlsl`](file:///C:/code/repro-vr/src/d3d12/frame_synthesis.hlsl).

### 2. Cartes de Rejet de Confiance (*Confidence & Specular Rejection*)
* **Problème :** Sur les surfaces transparentes (eau, vitres, fumée) ou les reflets spéculaires intenses, les vecteurs de mouvement du moteur (ou le flux optique) indiquent le mouvement de la surface géométrique alors que l'œil perçoit le reflet qui bouge différemment.
* **Ce qu'on cherche dans Quest 3 :** Comment Meta calcule une "carte de confiance" spatio-temporelle pour repérer ces aberrations et basculer en reprojection purement orientationnelle (3-DOF ATW) sur ces zones pour éliminer le déchirement visuel.
* **Transposition ReproVR :** Pondération dans le solveur d'occlusion ASW.

### 3. Foveated SpaceWarp / Variable-Rate Warping
* **Lien direct avec Cheeky DLSS Pimax :**
  * Le Quest 3 gère le rendu fovéaté dynamique avec son écran LCD et sa caméra de suivi.
  * Comment leur pipeline SpaceWarp adapte-t-il la densité d'échantillonnage de la reprojection entre le fovéa central et la périphérie ?
  * Récupérer leur formule de transition de bordure (`TransitionWidth`, `Roundness`) pour la calquer idéalement avec les profils `CheekyFoveatedDLSS.ini` de votre Pimax.

### 4. Splatting / Push-Pull Inpainting
* **Comblement de trous :** Quand une surface est complètement masquée dans les deux yeux (zone de désocclusion totale), quelle est la formule de remplissage de fond ? (Interpolation bilatérale guidée par la profondeur, ou extension directionnelle le long du gradient de texture ?).

---

## 5. Matrice de Passage : Du Quest 3 à ReproVR

| Découverte dans Quest 3 (Vulkan / SPIR-V / ARM64) | Équivalent Cible dans ReproVR (D3D12 / HLSL / x64) |
| :--- | :--- |
| Shader Vulkan AppSW `subpass` | Shader HLSL [`frame_synthesis.hlsl`](file:///C:/code/repro-vr/src/d3d12/frame_synthesis.hlsl) (`SynthesizeMidpointPS`) |
| Mathématique d'extrapolation quaternion Quest | Moteur d'extrapolation [`extrapolate_pose_6dof`](file:///C:/code/repro-vr/src/core/depth_reprojection.cpp#L189-L261) |
| Masques de silhouettes et pondérations de confiance | Constantes [`SynthesisConstants`](file:///C:/code/repro-vr/src/d3d12/frame_synthesis.hlsl#L8-L55) et passes de calcul |
| Presets de fovéation optique | Intégration transparente avec le profil Pimax de Cheeky |

---
*Ce plan d'investigation est prêt à être exécuté dès que le Quest 3 sera connecté en ADB.*
