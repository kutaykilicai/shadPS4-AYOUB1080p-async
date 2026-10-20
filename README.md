<!--
SPDX-FileCopyrightText: 2026 shadPS4-AYOUB1080p contributors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4-AYOUB1080p

A fork of the [shadPS4](https://github.com/shadps4-emu/shadPS4) PlayStation 4 emulator, tuned to run
**inFAMOUS Second Son** smoothly.

[Türkçe](#türkçe) · [English](#english) · [Français](#français)

> **Credits:** this repository is built on [shadPS4](https://github.com/shadps4-emu/shadPS4) by the
> shadPS4 team and contributors, and on the shadPS4-AYOUB1080p build by Ayoub
> ([@weshayoub92](https://github.com/weshayoub92)). The `-async` changes (pipeline cache loaded on a
> background thread, bounded waits) are by Kutay Kılıç. License: GPL-2.0-or-later, same as upstream.

> **Unofficial, independent project.** It is not affiliated with, approved, endorsed or sponsored by
> Sony Interactive Entertainment, Sucker Punch Productions or the shadPS4 team. PlayStation, PS4 and
> inFAMOUS are trademarks of their respective owners.
>
> This project contains **no game files, no keys, no firmware, no system modules and no shader
> caches**, and never will. Use it only with games you own, dumped from your own console.
>
> **Projet indépendant et non officiel**, non affilié ni approuvé par Sony Interactive Entertainment,
> Sucker Punch Productions ou l'équipe de shadPS4. Aucun fichier de jeu, aucune clé, aucun firmware,
> aucun module système ni cache de shaders. À utiliser uniquement avec vos propres jeux, extraits de
> votre propre console.

## Türkçe

shadPS4 PS4 emülatörünün bir çatalı. Ayoub'un inFAMOUS Second Son için hazırladığı AYOUB1080p
sürümünün üzerine, pipeline önbelleğinin arka planda yüklenmesini ekledim. Böylece oyun açılırken
önbellek yüklenmesi beklenmiyor, eksik pipeline'lar ihtiyaç anında öne alınıyor.

- **Derleme:** aşağıdaki İngilizce bölümdeki adımlar geçerli (Windows, CMake, Visual Studio 2022 veya
  Clang). GitHub Actions'taki `windows.yml` aynı derlemeyi yapıyor.
- **Kullanım:** `shadps4.exe -g "<oyun klasörü>\CUSA00004\eboot.bin"`
- **Uyarı:** Resmi bir proje değildir; Sony, Sucker Punch veya shadPS4 ekibiyle bağı yoktur. Depoda
  oyun dosyası, anahtar, firmware ya da sistem modülü yoktur. Yalnızca kendi konsolunuzdan aldığınız,
  size ait oyunlarla kullanın.
- **Lisans:** GPL-2.0-or-later (shadPS4 ile aynı).

## English

### What is different from shadPS4

Based on shadPS4 commit `2338a06` (30 September 2026). The changes target the slowdowns of the city
in inFAMOUS Second Son:

- **GPU readbacks** (the game reading data written by the GPU): copies on a second GPU queue, and the
  game thread that needs the data waits for it instead of the emulated GPU; a game write over memory
  the GPU wrote no longer waits for that memory to be downloaded first.
- **Less work on the command processor thread**: Vulkan commands recorded by a thread of their own,
  buffer uploads and page protections done by a worker thread, page protections of different threads
  in parallel.
- **Less repeated work per draw**: pipeline, shader, texture and render target lookups remembered,
  redundant binds and empty draws skipped, small constant buffers reused instead of copied again.
- **Shader compilation**: new graphics pipelines are compiled by worker threads; a draw whose pipeline
  is not ready after a few milliseconds is skipped instead of freezing the game.
- **Accuracy**: real occlusion queries (car headlights no longer shine through buildings), barriers
  for compute shaders whose workgroups span several host subgroups (particle sorting), GPU clock read
  by the game paced like the console so its dynamic quality system keeps the detail.
- **CPU placement**: the GPU threads of the emulator run on the cores of the largest L3 cache (Ryzen
  with 3D V-Cache) when the L3 caches differ in size.
- **Built-in game fix**: the checkerboard rendering of the PS4 Pro mode, which shows a green screen in
  shadPS4, is turned off for inFAMOUS Second Son, independently of patch files and launchers.
- **Patch files**: a patch line can carry `OriginalHash`, a checksum of the bytes it replaces. On
  another version of the game the patch is skipped instead of corrupting the code, and the patch file
  does not need to contain any code of the game.

These options are switched on automatically for inFAMOUS Second Son (CUSA00004, CUSA00223). Other
games keep the default values (most options off). This build is only tested with inFAMOUS Second Son.

### Game patches

`user-files\patches\shadPS4-AYOUB1080p\inFAMOUS Second Son.xml` in the release zip, for the European game
CUSA00004 (APP_VER 01.00):

| Patch | Default | What it does |
| --- | --- | --- |
| Dynamic quality thresholds x2 | on | the game no longer lowers its quality (sun shadows, resolution) in busy scenes: the limits of its dynamic quality system are doubled |
| Anti-freeze guard (collision loop) | on | a collision query stops after 200 000 iterations instead of hanging the game |
| Bigger light list | on | light lists sized for native 4K (the GPU hangs at 4K without it) |
| Streaming: closest city blocks first | off | lower priority penalties in the streaming of the city blocks |
| PS4 Pro mode: native 3200x1800 without checkerboard | on | native 3200x1800 rendering in PS4 Pro mode |
| PS4 Pro mode: native 3840x2160 / 2560x1440 without checkerboard | off | heavier / lighter than 3200x1800, enable only one resolution patch |
| Native 2560x1440 / 3840x2160 (render and output) | off | base PS4 mode (PS4 Pro mode off), enable only one resolution patch |
| Wider field of view x1.15 / x1.30 | off | picture only, the game camera is unchanged, enable only one |

Switch a patch on or off with `isEnabled="true"` or `"false"` in the file, then restart the game.
The checkerboard rendering of the PS4 Pro mode shows a green screen in shadPS4: this build turns it off
by itself for this game, whatever the launcher or the patch files, so without a "PS4 Pro mode" patch
the Pro mode renders in native 1920x1080. Without the PS4 Pro mode (`neo_mode` false) the game runs in
1080p: switch the "PS4 Pro mode" patch off then.

### Install

1. Download the zip of the latest [release](../../releases) and extract it anywhere.
2. Copy the content of its `user-files` folder into your shadPS4 user folder, `%APPDATA%\shadPS4`
   (or the `user` folder next to `shadps4.exe` for a portable install). It adds:
   - `patches\shadPS4-AYOUB1080p\`: the game patches above;
   - `custom_configs\CUSA00004.json`: the settings this build is tested with (PS4 Pro mode, precise
     GPU readbacks, 512 MB of extra memory, pipeline cache). If you already have this file, add these
     values to yours.
3. Start the game with `shadps4.exe -g "<game folder>\CUSA00004\eboot.bin"`, or select this
   `shadps4.exe` in your launcher (check that the patches are enabled there).

The log (`%APPDATA%\shadPS4\log\shad_log.txt`) lists every patch applied ("Applied patch") or skipped.

### Performance tips

- This build is tuned and tested on a Ryzen 9 9950X3D. Emulation depends mostly on the CPU: fewer or
  slower cores give a lower and less stable framerate.
- The first sessions compile the shaders of the game: expect stutters and drops during the first
  minutes in each new area. They are kept for the next launches (pipeline cache, on in the per-game
  settings).
- Lighter rendering: in the patch file, switch "PS4 Pro mode: native 3200x1800" off and "PS4 Pro mode:
  native 2560x1440" on, or turn the PS4 Pro mode off (`neo_mode` false) for 1080p and switch the "PS4
  Pro mode" patch off too.
- `perf_log = 1` in `perf_tuning.ini` (user folder) writes performance reports to the log, useful when
  you ask for help.

### Advanced settings

- `perf_tuning.ini` in the user folder changes any performance option while the game runs (list and
  descriptions in [`src/core/perf_sampler.h`](src/core/perf_sampler.h)); `perf_log = 1` writes
  performance reports to the log.
- Environment variable `SHADPS4_GAME_PROFILE=0`: start without the inFAMOUS Second Son settings and
  built-in fix.

### Build from source

As for shadPS4: clone with the submodules (`git clone --recursive`), then follow
[documents/building-windows.md](documents/building-windows.md) (Linux and macOS: the other files of
`documents`). Releases are built from the commit of their tag with clang-cl, CMake and Ninja
(`-DCMAKE_BUILD_TYPE=Release`).

### License

GPL-2.0-or-later, like shadPS4 ([LICENSE](LICENSE)). The source code of a release is the commit of its
tag in this repository.

### Credits

- The [shadPS4](https://github.com/shadps4-emu/shadPS4) team and contributors, who make the emulator.
- Changes by Ayoub ([@weshayoub92](https://github.com/weshayoub92)), developed with the help of Claude
  (Anthropic).

## Français

### Ce qui change par rapport à shadPS4

Basé sur le commit `2338a06` de shadPS4 (30 septembre 2026). Les changements visent les
ralentissements de la ville dans inFAMOUS Second Son :

- **Relectures GPU** (le jeu lit des données écrites par le GPU) : copies sur une seconde file GPU, et
  c'est le thread du jeu qui a besoin des données qui les attend, pas le GPU émulé ; une écriture du
  jeu sur de la mémoire écrite par le GPU n'attend plus que cette mémoire soit d'abord rapatriée.
- **Moins de travail sur le thread du processeur de commandes** : commandes Vulkan enregistrées par
  un thread dédié, envois de tampons et protections de pages faits par un thread de travail,
  protections de pages de plusieurs threads en parallèle.
- **Moins de travail répété à chaque dessin** : recherches de pipelines, de shaders, de textures et de
  cibles de rendu mémorisées, liaisons redondantes et dessins vides ignorés, petits tampons de
  constantes réutilisés au lieu d'être recopiés.
- **Compilation des shaders** : les nouveaux pipelines graphiques sont compilés par des threads
  dédiés ; un dessin dont le pipeline n'est pas prêt après quelques millisecondes est sauté au lieu de
  figer le jeu.
- **Précision** : vrais tests d'occlusion (les phares des voitures ne traversent plus les bâtiments),
  barrières pour les shaders de calcul dont les groupes couvrent plusieurs sous-groupes de l'hôte (tri
  des particules), horloge GPU lue par le jeu au rythme de la console pour que son système de qualité
  dynamique garde les détails.
- **Placement sur le processeur** : les threads GPU de l'émulateur tournent sur les cœurs du plus grand
  cache L3 (Ryzen avec 3D V-Cache) quand les caches L3 n'ont pas la même taille.
- **Correctif intégré** : le rendu en damier du mode PS4 Pro, qui donne un écran vert dans shadPS4, est
  coupé pour inFAMOUS Second Son, indépendamment des fichiers de patchs et des lanceurs.
- **Fichiers de patchs** : une ligne de patch peut porter `OriginalHash`, une somme de contrôle des
  octets qu'elle remplace. Sur une autre version du jeu, le patch est ignoré au lieu d'abîmer le code,
  et le fichier de patchs n'a besoin de contenir aucun code du jeu.

Ces options s'activent automatiquement pour inFAMOUS Second Son (CUSA00004, CUSA00223). Les autres jeux
gardent les valeurs par défaut (la plupart des options désactivées). Cette build n'est testée qu'avec
inFAMOUS Second Son.

### Patchs du jeu

`user-files\patches\shadPS4-AYOUB1080p\inFAMOUS Second Son.xml` dans le zip de la release, pour le jeu
européen CUSA00004 (APP_VER 01.00) :

| Patch | Par défaut | Effet |
| --- | --- | --- |
| Dynamic quality thresholds x2 | activé | le jeu ne baisse plus sa qualité (ombres du soleil, résolution) dans les scènes chargées : les limites de son système de qualité dynamique sont doublées |
| Anti-freeze guard (collision loop) | activé | un calcul de collisions s'arrête au bout de 200 000 tours au lieu de figer le jeu |
| Bigger light list | activé | listes de lumières dimensionnées pour la 4K native (sans lui, le GPU se bloque en 4K) |
| Streaming: closest city blocks first | désactivé | priorités du chargement des blocs de la ville moins pénalisantes |
| PS4 Pro mode: native 3200x1800 without checkerboard | activé | rendu natif en 3200x1800 en mode PS4 Pro |
| PS4 Pro mode: native 3840x2160 / 2560x1440 without checkerboard | désactivé | plus lourd / plus léger que 3200x1800, n'activer qu'un seul patch de résolution |
| Native 2560x1440 / 3840x2160 (render and output) | désactivé | mode PS4 de base (mode PS4 Pro désactivé), n'activer qu'un seul patch de résolution |
| Wider field of view x1.15 / x1.30 | désactivé | image seulement, la caméra du jeu ne change pas, n'en activer qu'un |

Pour activer ou désactiver un patch : `isEnabled="true"` ou `"false"` dans le fichier, puis relancer le
jeu. Le rendu en damier du mode PS4 Pro donne un écran vert dans shadPS4 : cette build le coupe
d'elle-même pour ce jeu, quels que soient le lanceur et les fichiers de patchs ; sans patch « PS4 Pro
mode », le mode Pro rend donc en 1920x1080 natif. Sans le mode PS4 Pro (`neo_mode` à false), le jeu tourne
en 1080p : désactiver alors le patch « PS4 Pro mode ».

### Installation

1. Télécharger le zip de la dernière [release](../../releases) et l'extraire où vous voulez.
2. Copier le contenu de son dossier `user-files` dans votre dossier utilisateur de shadPS4,
   `%APPDATA%\shadPS4` (ou le dossier `user` à côté de `shadps4.exe` pour une installation portable).
   Il ajoute :
   - `patches\shadPS4-AYOUB1080p\` : les patchs du jeu ci-dessus ;
   - `custom_configs\CUSA00004.json` : les réglages avec lesquels cette build est testée (mode PS4 Pro,
     relectures GPU précises, 512 Mo de mémoire en plus, cache de pipelines). Si vous avez déjà ce
     fichier, ajoutez-y ces valeurs.
3. Lancer le jeu avec `shadps4.exe -g "<dossier du jeu>\CUSA00004\eboot.bin"`, ou choisir ce
   `shadps4.exe` dans votre lanceur (vérifiez que les patchs y sont activés).

Le journal (`%APPDATA%\shadPS4\log\shad_log.txt`) indique chaque patch appliqué (« Applied patch ») ou
ignoré.

### Conseils de performance

- Cette build est réglée et testée sur un Ryzen 9 9950X3D. L'émulation dépend surtout du processeur :
  moins de cœurs, ou des cœurs plus lents, donnent un framerate plus bas et moins stable.
- Les premières sessions compilent les shaders du jeu : attendez-vous à des saccades et des chutes
  pendant les premières minutes dans chaque nouvelle zone. Ils sont gardés pour les lancements suivants
  (cache de pipelines, activé dans les réglages du jeu).
- Rendu plus léger : dans le fichier de patchs, désactiver « PS4 Pro mode: native 3200x1800 » et activer
  « PS4 Pro mode: native 2560x1440 », ou couper le mode PS4 Pro (`neo_mode` à false) pour du 1080p en
  désactivant aussi le patch « PS4 Pro mode ».
- `perf_log = 1` dans `perf_tuning.ini` (dossier utilisateur) écrit des rapports de performance dans le
  journal, utiles pour demander de l'aide.

### Réglages avancés

- `perf_tuning.ini` dans le dossier utilisateur modifie n'importe quelle option de performance pendant
  le jeu (liste et descriptions dans [`src/core/perf_sampler.h`](src/core/perf_sampler.h)) ;
  `perf_log = 1` écrit des rapports de performance dans le journal.
- Variable d'environnement `SHADPS4_GAME_PROFILE=0` : démarrer sans les réglages ni le correctif intégré
  d'inFAMOUS Second Son.

### Compiler depuis les sources

Comme pour shadPS4 : cloner avec les sous-modules (`git clone --recursive`), puis suivre
[documents/building-windows.md](documents/building-windows.md) (Linux et macOS : les autres fichiers de
`documents`). Les releases sont compilées depuis le commit de leur tag avec clang-cl, CMake et Ninja
(`-DCMAKE_BUILD_TYPE=Release`).

### Licence

GPL-2.0 ou ultérieure, comme shadPS4 ([LICENSE](LICENSE)). Le code source d'une release est le commit
de son tag dans ce dépôt.

### Remerciements

- L'équipe et les contributeurs de [shadPS4](https://github.com/shadps4-emu/shadPS4), qui font
  l'émulateur.
- Changements par Ayoub ([@weshayoub92](https://github.com/weshayoub92)), développés avec l'aide de
  Claude (Anthropic).
