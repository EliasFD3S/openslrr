# OpenSLRR

Host C++ pour **Street Legal Racing: Redline**. Il charge les classes TUFA du jeu, exécute les arbres TREE, et fournit les natives, les ressources, le rendu et l'audio que ces scripts appellent.

Le périmètre validé couvre le démarrage, les menus, les dialogues et le garage, y compris l'assemblage d'une voiture depuis ses CFG. La conduite, le monde et le trafic restent à faire. Les données du jeu ne sont pas dans ce dépôt.

La validation utilise **SLRR 2.2.1 MWM**. Win32 est la configuration de référence : les scripts et les codecs AVI 32 bits s'y attendent. Le backend bgfx est compilé en C++20.

## État

| Domaine | En place |
| --- | --- |
| Scripts | Classes TUFA, arbres TREE, appels Java et natifs, héritage, `switch` |
| Ressources | RPK et références entre packs, géométrie SCX, textures DDS/PTX, propriétés CFG |
| Rendu | bgfx/D3D11, garage texturé, caméra orbitale, instances de scène, skydome |
| Voiture | Création depuis un RPK/CFG, pièces d'origine, slots et alias de compatibilité. Référence : Baiern, 76 pièces |
| Catalogue | Catégories entre RPK, classes, prix, pièces et aperçus 3D dans les pages d'origine |
| Matériaux | Peinture par texture CFG, UV multiples, couche secondaire, envmap statique choisie par les scripts |
| Interface | Menus, dialogues de carrière, Club Info, track selector, champs texte, hover, DAY/TIME |
| Peinture | Cinq bombes 3D, couleurs issues des scripts, défilement, caméras d'inventaire séparées |
| Audio | Effets d'interface, `SfxRef`, voix avec volume, pitch, boucle et arrêt |
| FMV | AVI dans le rendu, piste audio des intros Activision/Invictus |

Dans les données de test, `Prime.avi` n'a pas de piste audio.

**Encore à développer :** conduite et physique intégrée, pose réelle des suspensions, transparences et éclairage complets, réflexions dynamiques, monde et trafic. Des natives sont encore des stubs. L'affichage du catalogue est testé ; l'achat, le montage interactif et la sauvegarde de ce cycle ne le sont pas. Pacejka est une sonde (`--probe-pacejka`), pas la boucle de conduite. Le multijoueur est hors périmètre.

## Provenance

Les scripts TUFA et les assets installés restent la source des classes, des pièces et de leurs paramètres. Le host réécrit les contrats en C++. Les dépôts suivants ont servi de spécification :

| Référence | Rôle pour ce host |
| --- | --- |
| [SLRR-JVM-decompiler](https://github.com/nedobylskiy/SLRR-JVM-decompiler) | Spec TUFA et `OP_FLAGS`, reprise dans `src/jvm/tufa.cpp` |
| [SLRR-JVM-compiler](https://github.com/nedobylskiy/SLRR-JVM-compiler) | Écriture TUFA, utile pour régénérer des classes |
| [SLRR-JVM](https://github.com/nedobylskiy/SLRR-JVM) | Dialecte source et natives, référence de comportement |
| [SLRR_rev](https://github.com/nedobylskiy/SLRR_rev) | Formats RPK/SCX, CFG, FindFile |
| [SLRR_rev `codex/physics-reverse-port` @ c65675f](https://github.com/nedobylskiy/SLRR_rev/tree/c65675f) | Couches SCX, peinture, envmap. Contrats dans [REFERENCE_PORT.md](REFERENCE_PORT.md) |
| [SLRR_rev `codex/pacejka-research`](https://github.com/nedobylskiy/SLRR_rev) | Feuille Pacejka SL2, exposée par `--probe-pacejka` |
| [io_invo_scx](https://github.com/nonamesex/io_invo_scx) | Structure des meshes SCX |
| [bgfx](https://github.com/bkaradzic/bgfx) | Rendu D3D11 |

D'autres notes de SLRR_rev (WheelRef, usure, dégâts) restent de la lecture. Elles ne pilotent pas encore le jeu. [QUARRY.md](QUARRY.md) décrit l'espace de travail local (`repos/`, `sources/`, `game/`) dans lequel ces références ont été lues.

## Prérequis

- Windows, Visual Studio 2022, le SDK Windows et CMake.
- Git, pour récupérer bgfx.
- Une installation du jeu : classes, RPK, modèles, textures et sons. Le fichier `system/Scripts/lang/System.class` doit y être.

Les commandes partent de la racine de ce dépôt. Dans l'espace de travail de développement, ce dossier s'appelle `host/`.

## Compilation

```powershell
git clone https://github.com/EliasFD3S/openslrr.git
cd openslrr

git clone https://github.com/bkaradzic/bgfx.cmake.git third_party/bgfx.cmake
git -C third_party/bgfx.cmake checkout 5ea74ee66662daef4089695846d6322da0dde94e
```

Le dossier de jeu par défaut est `../game`. Un autre emplacement se passe avec `-GameRoot` ou `--root`.

```powershell
cmake -S . -B build-bgfx -G "Visual Studio 17 2022" -A Win32 `
  -DSLRR_WITH_LIBBGFX=ON -DSLRR_BUILD_TESTS=ON -DSLRR_STAGE_BUILD=ON

.\validate-and-promote.ps1

# Installation ailleurs :
.\validate-and-promote.ps1 -GameRoot 'D:\Jeux\SLRR'
```

`validate-and-promote.ps1` compile dans `build-bgfx/candidate/`, lance les tests sur les assets d'origine, archive sources, empreintes SHA-256, journal et exécutables dans `../checkpoints/`, puis remplace `build-bgfx/Release/slrr_host.exe` seulement si les tests passent. L'ancien exécutable reste dans la sauvegarde. Fermer le jeu avant cette étape : un exécutable ouvert bloque le remplacement. Une compilation avec `SLRR_STAGE_BUILD=ON` ne met à jour que le candidat.

Les archives ne contiennent ni le jeu ni `third_party/`.

## Lancement

```powershell
# Intros, puis menus.
.\build-bgfx\Release\slrr_host.exe --game --render bgfx --root ..\game

# Garage, sans passer par la création de carrière.
.\build-bgfx\Release\slrr_host.exe --game --auto-new --render bgfx --root ..\game

# Baiern assemblée depuis ses scripts et CFG.
.\build-bgfx\Release\slrr_host.exe --game --spawn-car cars/racers/Baiern.rpk:0x6 --render bgfx --root ..\game

# Chargement des données, sans session interactive.
.\build-bgfx\Release\slrr_host.exe --probe --root ..\game

# Sonde Pacejka.
.\build-bgfx\Release\slrr_host.exe --probe-pacejka
```

`$env:SLRR_GAME_ROOT` fixe aussi le dossier de jeu. `--root` a la priorité. `slrr_host.exe --help` liste le reste.

Dans le garage : clic droit et glisser pour l'orbite, flèches et molette pour la caméra. Échap passe les intros. Fermer la fenêtre quitte.

`--spawn-car PACK:ID_LOCAL` ouvre une session de développement sans le dialogue de carrière. La pose est statique.

## Tests

```powershell
cmake --build build-bgfx --config Release `
  --target host_runtime_test ui_state_test scene_instances_test part_config_test --parallel
ctest --test-dir build-bgfx -C Release --output-on-failure
```

Avec les données de référence, CTest enregistre **16 tests** :

- effets simultanés, `money.wav`, boucles et arrêt des voix ;
- connexion et lecture de la piste audio des intros ;
- Tune et les `switch` des scripts ;
- HUD DAY/TIME, y compris après recréation ;
- dialogues de carrière, libellés et défilement ;
- descriptions du track selector, sans accumulation ;
- hover, boutons activés ou désactivés, focus ;
- cinq paint cans grises, page rouge suivante, disparition à la fermeture ;
- transformations, scènes d'inventaire isolées, skydome ;
- CFG, slots, alias, assemblage stock exécuté par les scripts ;
- pièces du catalogue, classes, prix, aperçus ;
- couches SCX, canaux UV, propagation de `Part.setTexture`.

`SLRR_TEST_GAME_ROOT` doit pointer vers le jeu pour les tests d'intégration. Sans ces données, seuls les tests autonomes sont construits. La promotion refuse une suite partielle.

Ces tests ne couvrent pas un cycle complet de chargement de carrière ni une session de conduite. Les tests audio vérifient la création des voix et l'état de lecture. Les effets positionnels passent par le mixage 2D.

## Organisation

```text
include/slrr/     Interfaces
src/jvm/          Parseur TUFA, interpréteur TREE, résolution des appels
src/natives/      Natives exposées aux scripts
src/resources/    RPK, SCX, textures, CFG, instances, assemblage
src/render/       Backends et polices
src/ui/           Widgets, focus, hover, heure
src/platform/     Entrées, audio, caméra, FMV
src/boot/         Démarrage et boucle principale
src/physics/      Sonde Pacejka
shaders/          Shaders bgfx
tests/            Tests autonomes et intégration
third_party/      Dépendances locales, ignorées par Git
assets/           Curseurs et icône extraits pour l'hôte
```

`game/`, `game2/`, `sources/`, `sources2/`, `repos/` et `docs/` vivent à côté de ce dépôt. Ils ne sont pas versionnés ici.
