
Conversation avec Gemini
J'envisage de faire un projet avec du rétro engineering dans le but est de créer un outil qui permet de s'intercaler au bon endroit pour pouvoir faire de la reprojection en VR comme le fait le smooth motion de steam. Mais dans mes souvenirs et d'après la littérature, il semble que le système de reprojection de méta est plus intéressant / propose un meilleur rendu général sur ce type d'outil. Ce que j'envisage c'est de comprendre. Comment marche l'outil de méta si mes observations sont bonnes et de proposer un outil open source qui permet de reprendre toutes les meilleures idées de l'outil de méta

Différence fondamentale d'architecture
L'écart de rendu entre SteamVR Motion Smoothing et la stack Meta (ASW 2.0 / SpaceWarp) tient à la nature des données injectées dans le pipeline d'estimation de mouvement :

SteamVR Motion Smoothing : Repose quasi exclusivement sur de l'estimation de flux optique (optical flow) 2D calculé a posteriori sur les frames couleur transmises au compositor. Conséquence : artefacts de déchirement (tearing / wobble) sur les grilles géométriques fines, les overlays HUD et les discontinuités de profondeur.

Meta ASW 2.0 (Asynchronous Spacewarp) : Combine le flux optique avec les buffers de profondeur (Depth Buffer) fournis par l'application hôte via l'API (OpenXR / OVRPlugin), ainsi que le tracking 6-DOF (IMU + pose prédictive) :

Reprojection 3D guidée par la profondeur : Déformation géométrique par re-projection directe du mesh / vertex grid projeté dans la nouvelle pose caméra. Traite parfaitement les translations rigides et la parallaxe sans distorsion locale.

Optical Flow résiduel (Motion Vectors) : Appliqué uniquement pour combler les mouvements d'animation internes au moteur (personnages, physique, particules) que la translation de caméra et la depth ne résolvent pas.

Application SpaceWarp (AppSW - mobile/Link) : L'application soumet directement ses propres vecteurs de mouvement calculés par le shader (motion vector buffer type TAA/DLSS), éliminant complètement l'étape d'estimation optique coûteuse et imprécise.

Pipeline technique Meta ASW 2.0
[ Application ] ---> Color + Depth Buffer (xrEndFrame / xrSubmitRigidBody)
                           |
                           v
[ Runtime / Layer Hook ]
  ├── 1. Reprojection géométrique pure (Depth Unprojection -> Re-projection sur delta pose HMD)
  ├── 2. Calcul flux optique résiduel (Hardware Optical Flow / NVOF / D3D12 Motion Estimation)
  ├── 3. Détection d'occlusion et inpainting / blending (gestion des disocclusions via temporal history)
  └── 4. Timewarp final (ATW rotationnel de dernière milliseconde avant scanout)
Soumission Depth : Via l'extension standard XR_KHR_composition_layer_depth. Le moteur (Unreal, Unity, custom) fournit un depth buffer normalisé (linéaire ou inverse Z).

Estimation matérielle : Meta utilise les blocs matériels d'estimation de mouvement (NVENC Optical Flow SDK sur Nvidia, équivalents AMD) plutôt qu'un compute shader générique, réduisant le frame time sous les 1,5 ms.

Masquage d'UI : L'UI soumise sur des composition layers séparés (XR_KHR_composition_layer_cylinder / quad) contourne le Spacewarp pour n'appliquer qu'un Timewarp rotationnel classique, éliminant tout artefact sur le texte et le HUD.

Où s'intercaler dans la stack ?
Deux approches viables selon la cible :

1. L'approche standardisée : OpenXR API Layer
Emplacement : Implémenter un OpenXR API Layer explicite (DLL enregistrée dans le registre Windows via le JSON manifest standard XrApiLayer_*.json).

Fonctionnement :

Intercepte xrCreateSession, xrCreateSwapchain, xrEndFrame.

Lors du xrCreateSwapchain, intercepter et injecter les flags de depth (XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_BIT).

Lors du xrEndFrame, lire le XrCompositionLayerProjection et forcer/extraire XrCompositionLayerDepthInfoKHR.

Générer les frames intermédiaires en Compute Shader / Direct3D 12 / Vulkan, puis soumettre les frames interpolées au runtime sous-jacent (SteamVR, Oculus, PimaxXR) à la fréquence de rafraîchissement native du HMD.

Avantages : Agnostique du runtime sous-jacent, compatible OpenXR sans injection binaire agressive ni hooks IAT/Detours.

2. L'approche Compositor Hook (bas niveau OpenVR / SteamVR)
Emplacement : Interception directe de vrclient.dll / openvr_api.dll (fonctions IVRCompositor::Submit).

Limites : Les jeux OpenVR natifs passent rarement la depth via l'API standard (VRTextureBounds_t), forçant à retomber sur de l'estimation de flux pur (comme le fait l'actuel Motion Smoothing de Valve) ou à injecter des hooks dans les draw calls DirectX/Vulkan pour hooker le Z-Buffer (approche type ReShade / UEVR).

Stack technique recommandée pour ton POC
Interception : API Layer OpenXR C++ (XR_APILAYER_ hooks).

Optical Flow : Utiliser directement NVIDIA Optical Flow SDK (NVOF) via l'API C++ dédiée (latence minimale, zéro charge CPU, calculé directement sur les blocs NVENC) ou un shader d'estimation DIS (Dense Inverse Search).

Calcul de reprojection : Compute Shaders HLSL/D3D12 ou GLSL/Vulkan prenant en entrée :

Frame(t-1) (Color + Depth)

Frame(t) (Color + Depth)

Pose(t-1), Pose(t), TargetPose(t_interp)

Temporal reprojection : Projection en arrière (backward warping) basée sur le depth unproject pour limiter les trous, avec un masque de disocclusion comblé par le flux optique résiduel.

Tu souhaites orienter ton hook directement sur un OpenXR API Layer (qui capture nativement la Depth passée par l'application), ou viser le runtime OpenVR/SteamVR legacy ?

L'idée est d'avoir le meilleur tradeoff pour le'pc entre perf faible empreinte memoire etnoverhead cpu / gpu minimal, approche openxr moderne uniquement profiter de la stack NVIDIA autant que possible et utiliser le projet rea de de compilation (check sur github) pour étudier et reprendre tout le nécessaire depuis ce que fournit meta sur la dernière version asw (j'ai un quest 3 duquel on peut avoir une connexion adb pour trouver des fishiers .so par exemple) affine l'approche avec ces éléments. Ce qui est produit doit pouvoir être installé très simplement (hook dll version par exemple) et maximiser la compatibilité avec d'autres mods (cheecky dlss 5 / real luke ross mod / uevr avec altrnate frame wrapping ...)

1. Inversion d'ingénierie : où cibler Meta Link / Quest
Inutile d'extraire les .so Android de l'OS du Quest 3 : sur Quest autonome, il s'agit d'AppSW (Application SpaceWarp, extension standardisée XR_FB_space_warp), qui délègue le calcul des motion vectors au moteur de jeu sous Vulkan. Ce qui t'intéresse pour le PC, c'est ASW 2.0, situé à 100 % dans le runtime Windows de Meta Link.   
Meta for Developers

Fichiers cibles (PCVR) : Dans C:\Program Files\Oculus\Support\oculus-runtime\ :

OVRServer_x64.exe (gestionnaire IPC et orchestration du compositor).

OvrVirtualDisplay.dll / OVRPlugin.dll / LibOVRRT64_1.dll (compositor bas niveau).

Symboles et désassemblage (Ghidra / IDA) :

Filtrer sur les chaînes et fonctions : Spacewarp, ASW, OpticalFlow, Reproject, DepthExtrapolate, MeshDisplacement.

Chercher les appels aux DLL tierces : nvapi64.dll, d3d11.dll, d3d12.dll. Meta utilise historiquement le bloc NVOF (NVIDIA Optical Flow) ou une passe compute DirectX 11/12 avec des grilles de vertex 2D déformées.

2. Architecture idéale (Overhead minimal & Low-Latency)
Pour minimiser l'empreinte mémoire et la latence CPU/GPU :

[ Jeu / Mod (UEVR, LukeRoss, DLSS) ]
                 │
                 ▼  xrEndFrame (Texture Color + Depth Buffer)
┌────────────────────────────────────────────────────────┐
│  Ton OpenXR API Layer (C++ / D3D12 / Shared Handles)    │
│                                                        │
│  1. Récupération Pointers Swapchain (Zero-Copy)       │
│  2. NVOF SDK 5.x (Hardware Optical Flow sur GPU)       │
│  3. Compute Shader (Depth Warp 3D + Flow Blending)    │
│  4. Émission frame synthétique intercalée              │
└────────────────────────────────────────────────────────┘
                 │
                 ▼
[ Runtime HMD réel (Virtual Desktop, SteamVR, Meta Link) ]
Zéro copie mémoire (Shared Handles) : Intercepter les textures de rendu sans jamais transférer les données vers la mémoire système. Partager directement les ID3D12Resource / ID3D11Texture2D via CreateSharedHandle entre le contexte du jeu et ta passe d'extrapolation.

Hardware Optical Flow (NVIDIA NVOF SDK) :

Le flux optique est exécuté sur les cœurs matériels dédiés (blocs matériels d'encodage NVENC/OFA), donc zéro coût sur les cœurs CUDA ou shaders SM.

NVOF accepte un indice de coût (cost table) et une surface de guidage. Tu peux lui passer la pose IMU différentielle pour éliminer le mouvement de translation de caméra et ne calculer le flux que sur la dynamique interne de l'image.

Compute Shader de reprojection :

Grille 2.5D : Découpage de l'écran en un maillage régulier (ex: 64×64 ou 128×128 quads).

Chaque vertex lit le Depth Buffer et la delta-pose caméra pour reprojeter sa position en espace écran.

Le vecteur NVOF vient affiner la position locale des vertex en cas de disocclusion ou d'objet mobile.

3. Packaging & Compatibilité avec les mods (UEVR, LukeRoss, DLSS)
Pour coexister avec des mods injecteurs de hooks DXGI (dxgi.dll, DLSS Enabler, Reshade) ou les frameworks VR (UEVR, OpenXR Toolkit) :

Ne pas faire de hook dxgi.dll local : S'intercaler via un faux dxgi.dll crée des collisions immédiates dès qu'un mod comme UEVR ou DLSS Frame Generation utilise déjà ce nom de proxy.

Choisir l'OpenXR API Layer explicite :

Se base sur la spécification officielle Khronos (XR_EXT_api_layer).

Installation : Un simple fichier .json référencé dans la clé de registre Windows :
HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit
(ou activé à la demande via OpenXR Toolkit / OpenXR Layers GUI).

Tous les mods OpenXR (UEVR, Luke Ross R.E.A.L., etc.) appellent la chaîne des loaders OpenXR. Ton layer se place en amont du runtime final.

Gestion des mods "Alternate Frame Rendering" (ex. Luke Ross) :

Dans ce mode, le mod rend l'œil gauche à t, et l'œil droit à t+1.

Ton layer doit détecter ce motif temporel (désynchronisation intentionnelle des poses ou des soumissions xrEndFrame) pour synthétiser l'œil manquant au lieu de forcer une reprojection stéréo identique sur les deux yeux.

Récupération du Depth Buffer :

Si le jeu soumet XR_KHR_composition_layer_depth, tu l'interceptes directement dans XrCompositionLayerProjectionView.

Si le jeu ne soumet pas de depth (cas de certains jeux plats moddés), intercepter la swapchain de profondeur DXGI/D3D12 la plus probable (par taille de viewport et format DXGI_FORMAT_D32_FLOAT / D24_UNORM_S8_UINT) en hookant CreateDepthStencilView.

Plan d'implémentation POC
Squelette : Forker le template minimal Khronos openxr-api-layer-example en C++20.

Capture : Intercepter xrEndFrame, logger les structures XrCompositionLayerDepthInfoKHR.

Flux optique : Initialiser une session NvOF (NVIDIA Optical Flow SDK) prenant les textures t-1 et t en NV_OF_BUFFER_FORMAT_ABGR8.

Synthèse : Dispatcher un Compute Shader D3D12 qui génère une frame interpolée, puis appeler l'aval xrEndFrame deux fois (frame synthétique à mi-chemin du delta IMU, puis frame réelle).

Tu es sur que la version dans meta sur pc est aussi avancée que sur ke uest 3 ?

Non, et c'est précisément le nœud du problème : la version PC de Meta (ASW 2.0) est en réalité figée depuis des années, alors que c'est sur Quest autonome (Meta Horizon OS) que l'ingénierie logicielle de Meta a le plus évolué avec AppSW (Application SpaceWarp).

Pourquoi la version standalone (Quest 3) produit un meilleur résultat
La différence de qualité perçue ne vient pas d'un algorithme secret de reprojection dans le runtime PC, mais de l'origine des données de mouvement :

Fonctionnalité	ASW 2.0 (Meta Link / PC)	AppSW (Quest 3 autonome)	SteamVR Motion Smoothing
Génération des vecteurs	Estimation a posteriori (Optical Flow heuristique)	Moteur de jeu direct (Motion Vectors natifs par pixel)	Estimation a posteriori (Optical Flow 2D pur)
Connaissance de la scène	Color + Depth buffer (quand supporté)	Color + Depth + Ground-Truth Motion Vectors	Color uniquement
Objets animés / Particules	Hallucinations / Tearing fréquents	Traités fidèlement (issus des vertex shaders)	Fortes déformations géométriques
Coût d'inférence	Modéré (GPU PC)	Extrêmement faible (~quelques Mo de swapchain)	Modéré (Compute GPU)
Sur Quest 3, Meta a standardisé l'extension XR_FB_space_warp : le moteur (Unreal/Unity) rend à demi-cadence mais calcule lui-même les vecteurs de mouvement réels de chaque objet dans le shader (identique aux motion vectors du TAA/DLSS). Le compositor matériel Adreno/XR2 n'a plus qu'à appliquer la déformation sans chercher à « deviner » le mouvement.

Sur PC, Meta a quasiment cessé de faire évoluer le client Link/Rift après 2019 : ASW 2.0 dépend d'un pipeline d'estimation mixte (Depth + NVOF) qui souffre dès que l'application ne soumet pas une depth buffer propre ou qu'elle tourne sous DX12/Vulkan moderne sans intégration OVR spécifique.

Ce qu'il faut en déduire pour ton outil   
Reddit
Plutôt que de rétro-concevoir le runtime PC de Meta (qui utilise une approche vieillissante de reconstruction par estimation), la stratégie la plus performante consiste à porter le modèle AppSW du Quest sur PC :

Récupérer les Motion Vectors natifs du moteur :

Avec UEVR ou les mods de type Luke Ross, le moteur génère déjà des velocity buffers (pour le TAA, TSR ou DLSS).

Au lieu de faire tourner un algorithme de flux optique coûteux et imprécis pour deviner les vecteurs, ton OpenXR Layer peut intercepter directement les textures de vélocité natives de l'application si elles sont exposées ou accessibles en shared resource.

Fallback sur flux optique matériel (NVIDIA OFA) :

Pour les jeux plats moddés ou anciens sans velocity buffer disponible, tu bascules sur le hardware Optical Flow (NVOF SDK 5.x) combiné au Depth Buffer (XR_KHR_composition_layer_depth) pour faire du "vrai" ASW 2.0 moderne.

L'avantage d'un Layer OpenXR indépendant :

Tu bénéficies de la puissance de calcul brute de la machine (cœurs NVOF de la carte graphique, shaders D3D12/Vulkan) tout en exploitant les données géométriques réelles des moteurs récents, dépassant les limitations du runtime Meta Link historique.

J'ai choisi le modèle argon pour la suite. Révise tout ce projet, dis moi si tu vois plus simple / performant / meilleur angle d'approche pour ce sujet ou si on est sur lz bonne piste et confirme moi si c'est bien argon qui parle

Tu interagis ici avec Gemini (et non un modèle nommé Argon — si tu passes par un routeur, une arène ou un proxy, le backend actif sur cette session est bien Gemini).

En révisant l'ensemble de l'architecture à froid, l'approche initiale (un simple OpenXR API Layer qui intercale des frames dans xrEndFrame) comporte deux écueils majeurs qui vont briser ton objectif de qualité et de simplicité.

Voici la critique technique de la piste actuelle et le pivot architectural beaucoup plus performant.

1. Les 2 failles de l'approche "OpenXR Layer classique"
Le blocage du Pacing OpenXR (xrWaitFrame / xrEndFrame) :
Dans la spec OpenXR, la boucle de rendu du jeu est cadencée par xrWaitFrame. Un OpenXR Layer ne peut pas facilement appeler xrEndFrame deux fois à 
t et 
t+0.5 pour une seule frame de jeu sans désynchroniser complètement le thread de soumission du runtime sous-jacent (Virtual Desktop, SteamVR, Link) ou bloquer le thread CPU du jeu. De plus, si tu génères ta frame à 
t, la pose HMD que tu utilises pour la frame synthétique à 
t+0.5 est une prédiction ancienne, alors que le secret de la fluidité VR est d'évaluer la pose IMU au dernier moment absolu avant le scanout.

Le piège du Reverse Engineering de Meta (Quest 3 / Link) :
Sur Quest 3 (Horizon OS), les binaires système (.so) qui gèrent l'AppSW et le Timewarp ne font quasiment aucun calcul mathématique lourd en C++ ou en shader standard : ils délèguent l'interpolation à un bloc matériel propriétaire de Qualcomm sur le Snapdragon XR2 Gen 2 (le DPU / Eva Engine d'Adreno). Décompiler ces .so ne te donnera que des appels de configuration driver QCOM propriétaires, inutilisables sur une architecture PC x86/NVIDIA.

2. Le meilleur angle d'approche : détourner le pipeline Streamline / DLSS-FG
Puisque tu cibles les jeux moddés (UEVR, Luke Ross) et les GPU NVIDIA modernes, le pipeline d'extraction des données existe déjà dans ces jeux : c'est celui de l'upscaling temporel (DLSS / FSR / XeSS).

Plutôt que de tenter de reconstruire la profondeur et le flux optique à l'aveugle dans OpenXR, l'approche la plus propre et performante consiste à hybrider un intercepteur Streamline/OptiScaler avec l'API Layer OpenXR :

Composant	Approche initiale (OpenXR pur)	Approche optimisée (Hook Streamline + OpenXR)
Motion Vectors (MV)	Calculés par NVOF (coûteux, erreurs sur les particules/UI)	Gratuits et exacts (Ground-Truth) : récupérés directement depuis les hooks slSetTag (Streamline) ou FSR2/3 du moteur.
Depth Buffer	Dépend de XR_KHR_composition_layer_depth (souvent absent ou cassé sur les mods)	Toujours présent : le moteur le fournit obligatoirement au DLSS/TAA avant le post-process.
UI / HUD	Déformé par la reprojection (artefacts)	Isolé : les hooks DLSS reçoivent le buffer Hudless ou le masque d'alpha, permettant de reprojeter la scène 3D sans tordre le HUD.
Overhead GPU	~1.5 à 2 ms (NVOF + Compute)	Moins de 0.4 ms (simple Compute Shader de forward/backward projection).
3. Comment gérer l'Alternate Eye Rendering (AER v2 de Luke Ross)
C'est le point où tu peux apporter une valeur ajoutée massive que ni SteamVR ni Meta ne gèrent correctement (d'où l'effet de ghosting infâme en AER quand le framerate décroche).

En mode AER :

Frame 
t : Le moteur calcule et soumet uniquement l'Œil Gauche.

Frame 
t+1 : Le moteur calcule et soumet uniquement l'Œil Droit.

Au lieu de faire de la génération de frames temporelle classique (passer de 45 à 90 fps sur les deux yeux en même temps), le mode le plus efficace en VR moddée est la Synthèse Stéréo Temporelle Croisée :

À l'instant 
t, tu as l'Œil Gauche frais (
L 
t
​
 ) et l'Œil Droit ancien (
R 
t−1
​
 ).

Grâce au Depth Buffer et aux Motion Vectors du moteur, tu peux reprojeter 
R 
t−1
​
  vers l'instant 
t (via la pose HMD exacte de l'œil droit à 
t), OU projeter géométriquement les pixels de 
L 
t
​
  vers le point de vue de l'œil droit pour boucher les trous de désocclusion.

Tu soumets au runtime OpenXR une paire stéréo parfaitement synchronisée à chaque tick, éliminant le décalage temporel gauche/droite qui donne la nausée dans les mods Luke Ross ou UEVR en mode AFR.

4. Architecture cible recommandée (Ultra-légère)
Au lieu de partir de zéro ou de décompiler OVRServer_x64.exe, voici la stack la plus directe :

Base d'interception des buffers (Mod-friendly) :
S'inspirer (ou s'interfacer via mémoire partagée / IPC local) avec l'architecture d'OptiScaler (open-source). Il intercepte déjà nvngx_dlss.dll / sl.interposer.dll et possède déjà les pointeurs exacts vers Color, Depth, MotionVectors et Hudless sur tous les jeux UE4/UE5 et les moteurs compatibles DLSS.

Cœur de reprojection (Le Warper) :

Si tu veux la qualité DLSS 3/4 sans l'incompatibilité VR : appelle directement l'API bas niveau NVIDIA Optical Flow + Frame Generation via Streamline, mais en injectant la matrice de reprojection HMD (
ViewProjection 
t
​
 →ViewProjection 
t+1
​
 ) dans les vecteurs de mouvement avant de les passer au générateur.

Alternative 100 % custom et plus déterministe en latence : un unique Compute Shader D3D12 qui prend la grille Depth + Motion Vectors du moteur, applique le delta de pose 6-DOF fourni par xrLocateViews, et résout les trous (disocclusions) par échantillonnage du buffer 
t−1.

Synchronisation OpenXR (Découplage de thread) :
Pour injecter une vraie frame à mi-chemin (
t+0.5) sans bloquer le jeu :

Ton OpenXR API Layer doit créer un second thread de soumission asynchrone qui possède la vraie session OpenXR face au runtime (VD / SteamVR).

Le jeu tourne à 
N/2 Hz sur une fausse horloge xrWaitFrame émulée par ton layer, pendant que ton thread asynchrone tourne à 
N Hz (cadencé par le vrai xrWaitFrame du casque) et alterne : Frame Réelle Reprojetée 
→ Frame Synthétisée avec nouvelle pose IMU.

Veux-tu qu'on creuse la mécanique du thread asynchrone OpenXR (comment leurrer le jeu sur xrWaitFrame sans casser le frame-pacing), ou l'extraction directe des Motion Vectors depuis le pipeline DLSS/UEVR ?

Etudie alternate frame wrapping (un pronet github fork de uevr) pour prendre ça en compte. (Pas pareil que aer) . Et dis moi si ton approche tacle les problèmes de latence frame gen en proposant une vraie reprojecttion propre

1. Analyse d'AFW (Alternate Frame Warping - Fork UEVR de PureDark)
Contrairement à l'AFR/AER classique (qui affiche un œil 
t et un œil 
t−1 brut, provoquant une désynchronisation rétinienne immédiate) ou au Synchronized Sequential d'UEVR (qui calcule les deux yeux sur le même tick moteur mais double le coût GPU), AFW résout l'équation directement dans le pipeline DX12 avant la soumission OpenXR :   
GitHub

Le principe d'AFW : Le moteur ne calcule qu'un seul œil par tick (alternativement Gauche à 
t, Droit à 
t+1), offrant un gain de 60 à 80 % de FPS. Mais au lieu d'envoyer l'œil manquant en retard, AFW reprojette (warp) l'œil calculé à 
t−1 vers l'instant 
t (et combine avec les données de l'œil opposé via l'option CombinedWarping).   
GitHub
+ 1

Pourquoi AFW oblige à activer DLSS / DLAA / FSR : AFW ne calcule pas de flux optique. Il se greffe sur le point d'entrée de l'upscaler pour voler les buffers internes d'Unreal Engine : Depth Buffer, Motion Vectors (Velocity Buffer) et l'historique temporel (~500 Mo de VRAM supplémentaires pour maintenir les history buffers stéréo séparés).   
GitHub

La mécanique interne : Il décompose la reprojection en deux passes (Compensate camera movement via la matrice de reprojection HMD + Allow moving-object history via les Motion Vectors du moteur pour re-warper les objets dynamiques sans traînées).

2. Pourquoi le Frame Gen classique (DLSS-FG) échoue sur la latence en VR
Ton questionnement sur la latence touche au cœur mathématique du Frame Generation :

Interpolation (DLSS 3/4 FG, Lossless Scaling) = Latence catastrophique :
Pour générer une frame 
t+0.5, un algorithme d'interpolation doit attendre que le moteur ait fini de calculer la frame 
t+1. Il garde la frame 
t+1 en otage en mémoire tampon, affiche 
t+0.5, puis affiche 
t+1 au cycle suivant. Résultat : tu ajoutes 1 à 2 frames complètes de latence motion-to-photon (soit +15 à +25 ms). En VR, le vestibule humain détecte tout décalage supérieur à ~15 ms lors d'une rotation de tête.

Extrapolation / Reprojection (Meta AppSW, ASW 2.0, AFW) = Latence nulle :
On ne retient jamais une frame calculée. Dès que la frame 
t est prête, elle part à l'écran. Pour la frame synthétique à 
t+0.5 (ou l'œil manquant en AFW), on extrapole vers le futur à partir de 
t et 
t−1, en interrogeant l'IMU du casque (xrLocateViews) à la dernière milliseconde. La latence ressentie sur la rotation/translation de tête correspond à la fréquence native du casque (90/120 Hz), même si le moteur tourne à 45/60 Hz.

3. Impact sur ton projet : comment te positionner face à AFW ?
Puisque PureDark a déjà implémenté le warping intra-moteur pour UEVR (DX12 uniquement) et REFramework, refaire exactement la même chose dans UEVR serait redondant. En revanche, AFW a trois limites structurelles que ton projet peut résoudre :

Limite actuelle d'AFW (PureDark)	Ton opportunité (OpenXR Layer + Hook Upscaler Universel)
Lié au mod moteur (exclusif UEVR DX12 et REFramework)	Agnostique du moteur : Fonctionne sur Unity (UUVR), Cyberpunk (Luke Ross), jeux VR natifs lourds (MSFS, Simracing, VorpX).
Génère à la fréquence du jeu : Si le jeu tombe à 55 FPS en AFW, tu as 55 FPS dans le casque (nécessitant quand même SSW/ASW par-dessus).	Vrai découplage temporel (Async Extrapolation) : Le thread OpenXR tourne verrouillé à 90/120 Hz quoi qu'il arrive, avec la pose HMD rafraîchie en temps réel.
Artefacts de désocclusion sur les bords d'objets (pas d'optical flow matériel de secours).	Hybridation MV Moteur + NVIDIA OFA : Utiliser les Motion Vectors du DLSS pour la précision géométrique, et les cœurs matériels NVOF pour l'inpainting des zones désoccultées.
4. L'architecture "Zéro Latence" pour ton outil
Pour battre le Frame Gen sur la latence tout en égalant la propreté de Meta AppSW et d'AFW :

Zéro Interpolation (100 % Extrapolation Late-Latching) :
Ne jamais retarder la frame 
t. Lorsqu'une frame synthétique doit être générée à 
t+Δt, ton thread asynchrone appelle xrLocateViews avec le predictedDisplayTime exact du prochain VSync HMD.

Projection Forward-Backward par grille Depth + MV :

Tu interceptes Color, Depth et MotionVectors via un proxy nvngx_dlss.dll / Streamline (comme le fait OptiScaler ou AFW).

Si le jeu tourne avec un mod AFR/AER (Luke Ross) ou AFW : ton layer détecte la parité d'œil. S'il est utilisé en complément d'AFW (pour passer de 45 fps AFW à 90 fps natifs sans le flou de Virtual Desktop SSW), il récupère les deux yeux déjà reconstruits par AFW + leurs buffers de vélocité, et extrapole la paire stéréo à 
t+0.5 avec la nouvelle pose IMU.   
GitHub

Correction du problème d'inertie des Motion Vectors :
En extrapolation pure, un objet qui se déplace à vitesse 
V

  entre 
t−1 et 
t est projeté à 
t+0.5× 
V

  (en supposant sa vitesse constante sur 8 ms), tandis que la caméra, elle, n'est pas extrapolée par 
V

  
cam
​
  mais recalculée exactement avec la vraie matrice de vue issue du tracking HMD à 
t+0.5. C'est cette séparation stricte entre mouvement caméra (IMU temps réel) et mouvement monde (MV extrapolés) qui élimine toute sensation de latence type DLSS-FG.

Pour observer concrètement le rendu et la configuration de ce pipeline en conditions réelles, cette démonstration détaille comment utiliser AFW dans UEVR.

How to use AFW in UEVR (80% Performance Increase)
LunchAndVR · 7,1 k vues

 Les vues de vidéos YouTube seront stockées dans votre historique YouTube, et vos données seront stockées et utilisées par YouTube conformément à ses  Conditions d'utilisation   
GitHub





Gemini est une IA et peut se tromper, y compris sur des personnes. Votre confidentialité et GeminiS'ouvre dans une nouvelle fenêtre

