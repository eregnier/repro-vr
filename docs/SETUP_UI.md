# Interface d'installation et de réglage

But : installer l'outil dans un jeu et régler ses paramètres depuis une petite
fenêtre, sans jamais exposer une valeur libre.

## Décision : Direct2D, pas de WebView

Mesuré sur la machine : le **runtime** WebView2 est présent
(`…\Microsoft\EdgeCore\155\msedgewebview2.exe`), mais **pas le SDK** — aucun
`WebView2.h`, aucune `WebView2Loader.lib` dans le cache NuGet ni sous
`Program Files`. L'utiliser demanderait de vendoriser un SDK entier et ajoutait une
dépendance de plusieurs centaines de Mo.

Retenu : **Win32 + Direct2D/DirectWrite**, composants fournis par Windows.
Un seul exécutable, aucune DLL annexe, rendu antialiasé et texte ClearType. Un
canevas dessiné à la main donne le style Flat/Material attendu : cartes arrondies,
segments, bascules, info-bulles.

**Entièrement portable** : runtime C lié statiquement (`MSVC_RUNTIME_LIBRARY
MultiThreaded`). L'exécutable ne dépend que de `KERNEL32`, `USER32`, `SHELL32`,
`ole32`, `d2d1`, `DWrite` — tous présents sur un Windows 10/11 nu. Aucun
redistribuable VC++ requis. Coût : 292 Ko au lieu de 124 Ko.

## Conséquence sur les contrôles

Tous les réglages réels du layer sont des **énumérations fermées** ou des
**bascules** : il n'existe donc aucune saisie libre dans l'interface. Une valeur
aberrante est impossible par construction, pas par validation.

| Réglage | Type | Valeurs |
| --- | --- | --- |
| `[ofxr] enabled` | bascule | on / off |
| `[ofxr] backend` | segments | FidelityFX / NVIDIA OFA |
| `[ofxr] motion_vectors` | segments | off / DLSS |
| `[ofxr] nvidia_preset` | segments | slow / medium / fast |
| `[ofxr] nvidia_input_scale` | segments | 100 / 75 / 50 |
| `[ofxr] nvidia_bidirectional` | bascule | on / off |
| `[ofxr] triple_frame_gen` | segments | 2 images (x2) / 3 images (x3) |
| `[ofxr] deep_pipeline` | bascule | on / off |
| `[ofxr] stereo_input_mode` | segments | auto / native / AER |
| `[ofxr] target_app_fps` | segments | auto / 45 / 60 / 72 / 90 |
| `[overlay] position` | segments | off / HG / HD / BG / BD |
| `[diagnostics] logging_enabled` | bascule | on / off |
| `[diagnostics] dump_guides` | bascule | on / off |

Chaque ligne porte une **info-bulle** qui explique ce que fait le réglage et ce
qu'il coûte. Pas de « valeur par défaut » implicite : elle est affichée et
restaurable.

## Modèle de stockage

- **Liste des installations** : `%APPDATA%\ReproVR\games.ini`, une section par jeu
  (`path`, `name`), écrite avec les API profil de Windows — aucun parseur maison.
- **Réglages d'un jeu** : `repro_vr.ini` **dans le dossier du jeu**, aux côtés de
  la DLL installée. Si `ofxr_bridge.ini` existe déjà (ancienne disposition), il est
  réécrit à l'identique pour ne pas laisser deux sources de vérité.
- Section `[ofxr]` par défaut ; le layer accepte aussi `[repro_vr]` et `[reprovr]`
  pour les entiers, la compatibilité est donc conservée.

## Installation

Copier dans le dossier du jeu, depuis le dossier de l'utilitaire :

1. `XR_APILAYER_XRFrameBridge_diagnostic.dll` ;
2. `XR_APILAYER_XRFrameBridge_diagnostic.json` (manifeste d'API layer implicite,
   chemin absolu mis à jour) ;
3. créer `repro_vr.ini` s'il n'existe pas.

Rien d'autre n'est modifié. Le registre global `HKLM\Software\Khronos\OpenXR` n'est
**pas** touché : le layer reste découvert par le manifeste posé à côté de
l'exécutable du jeu, la seule voie mesurée comme fonctionnelle ici.

Désinstallation : retirer la DLL et le manifeste, **conserver** le `repro_vr.ini`
(les réglages de l'utilisateur) et l'entrée dans la liste.

## Écran

- Colonne de gauche : jeux installés (nom + dossier), bouton « Ajouter un jeu »
  (sélecteur de dossier), retrait d'une entrée.
- À droite : nom du jeu, état d'installation, bouton primaire
  Installer / Désinstaller, puis les cartes de réglages groupées
  (Général, Reprojection, Performance, Diagnostic).

## Réglage à chaud

Les réglages peuvent être changés **pendant que le jeu tourne** : le layer relit
son ini au plus une fois toutes les 500 ms (`embedded::poll_ini`, appelé depuis
`apply_embedded_control`, donc une fois par `xrEndFrame`), et n'applique quelque
chose que si une clé a réellement changé. Aucun processus séparé, aucune injection,
aucune hotkey : l'outil écrit le fichier, le layer le remarque.

Sections lues, dans cet ordre de priorité : `[repro_vr]`, `[reprovr]`, `[ofxr]` —
le même ordre que les lecteurs du layer au démarrage. Une valeur illisible retombe
sur le défaut au lieu de devenir zéro.

Le fichier surveillé est celui que le layer a lu au démarrage :
`repro_vr.ini` s'il existe, `ofxr_bridge.ini` sinon.

## Comment l'essayer

```
ninja -C build reprovr_setup        (ou build_diag.bat)
build
eprovr_setup.exe
```

L'exécutable doit se trouver **dans le même dossier que la DLL et son manifeste**
(c'est de là qu'il copie). Ensuite : « Ajouter un jeu » → choisir le dossier qui
contient l'exécutable du jeu → bouton « Installer ».

Vérification : le dossier du jeu doit contenir la DLL, le manifeste JSON et
`repro_vr.ini`. Le bouton bascule en « Désinstaller » quand c'est le cas.

## Distribution

**Deux fichiers** : `reprovr_setup.exe` et
`XR_APILAYER_XRFrameBridge_diagnostic.dll`, côte à côte. Le manifeste `.json`
n'est pas livré : l'installeur le génère à l'installation, avec le chemin absolu
de la DLL qu'il vient de copier.

Le dossier `build/` n'est pas un livrable : il contient 85 Mo d'artefacts de test
(exécutables de contrat, fixtures, `.exp`/`.lib`, `.ninja`, `CMakeFiles`). Seul
`build/dist/` est produit pour l'usage, et il est régénéré à chaque compilation de
`reprovr_setup` :

```
ninja -C build reprovr_setup
build\dist\            <- les deux fichiers à garder
```

**Décision : pas d'auto-extraction.** Un binaire unique qui écrit une DLL sur le
disque puis la fait charger par un autre processus reproduit exactement la
signature d'un *dropper* : c'est un déclencheur heuristique classique des
antivirus, pour un gain de confort nul (trois fichiers dans un dossier au lieu
d'un). Le format « trois fichiers » est aussi celui qu'on peut inspecter et
signer séparément.

Ce qui reste à traiter avant une diffusion large : la **signature de code**
(absent aujourd'hui, donc SmartScreen signalera l'exécutable au premier
lancement), et le fait qu'un manifeste d'API layer pointant un chemin absolu vers
une DLL est un motif que certains moteurs regardent. Rien de tout cela n'affecte
l'usage local.

## Statut

- [x] schéma des réglages et bornes
- [x] liste d'installations et écriture d'ini
- [x] installation / désinstallation
- [x] fenêtre Direct2D, cartes, segments, bascules, info-bulles, défilement
- [ ] champ de recherche / renommage d'une entrée
- [ ] vérification à l'écran sur un vrai jeu (dossier d'un jeu non installé)
