# OpenSLRR

Host C++ expérimental pour **Street Legal Racing: Redline**. Le projet exécute les scripts compilés du jeu dans un interpréteur TUFA et fournit progressivement les fonctions natives, les ressources, le rendu et l'audio nécessaires.

Le travail actuel porte sur le démarrage, les menus et le garage. Ce n'est pas encore une réimplémentation complète et jouable du moteur.

## État actuel

La validation utilise les données de **SLRR 2.2.1 MWM**.

| Domaine | Fonctionnalités présentes |
| --- | --- |
| Scripts | Chargement des classes TUFA, exécution des arbres TREE, appels Java/natifs, héritage et instructions `switch` |
| Ressources | Lecture des RPK, résolution des références entre packs, géométrie SCX, textures DDS/PTX et lecture des propriétés CFG |
| Rendu | Backend bgfx/D3D11, garage texturé, caméra orbitale, instances de scène et skydome |
| Interface | Menus, dialogues de carrières, Club Info, track selector, champs texte, hover et affichage DAY/TIME |
| Peinture | Bombes de peinture 3D dans les cinq cases, couleurs issues des scripts, défilement et caméras d'inventaire indépendantes |
| Audio | Effets UI et appels `SfxRef`, voix indépendantes avec volume, pitch, boucle et arrêt |
| FMV | Lecture AVI, vidéo intégrée au rendu et sortie audio des intros Activision/Invictus |

Dans les données utilisées pour les tests, `Prime.avi` n'a pas de piste audio.

**Encore à développer :** assemblage et affichage d'une voiture complète depuis ses CFG et ses pièces, réflexions de carrosserie, conduite/physique intégrée, monde et trafic. Les natives restent incomplètes ; certaines fonctions sont encore des stubs. Le module Pacejka est une base expérimentale, pas une simulation de conduite opérationnelle. Le multijoueur est hors du périmètre actuel.

## Prérequis

- Windows et Visual Studio 2022 avec les outils C++, le SDK Windows et CMake.
- Git pour récupérer les dépendances de rendu.
- Une installation du jeu fournissant les classes, RPK, modèles, textures et sons.

**Win32 est la configuration de référence**, notamment pour les conventions des scripts et les codecs AVI 32 bits. Le backend bgfx utilise C++20. Les données du jeu ne sont pas fournies par ce dépôt.

Les commandes ci-dessous se lancent depuis la racine du dépôt `openslrr` — le dossier appelé `host/` dans l'espace de travail de développement.

## Installation et compilation

```powershell
git clone https://github.com/EliasFD3S/openslrr.git
cd openslrr

# Wrapper CMake de bgfx ; ses dépendances sont récupérées par CMake.
git clone https://github.com/bkaradzic/bgfx.cmake.git third_party/bgfx.cmake
git -C third_party/bgfx.cmake checkout 5ea74ee66662daef4089695846d6322da0dde94e
```

Placez les données du jeu dans un dossier voisin `../game/`, ou indiquez leur emplacement avec `-GameRoot` et `--root`. Le fichier `system/Scripts/lang/System.class` doit être présent dans ce dossier.

```powershell
cmake -S . -B build-bgfx -G "Visual Studio 17 2022" -A Win32 `
  -DSLRR_WITH_LIBBGFX=ON -DSLRR_BUILD_TESTS=ON -DSLRR_STAGE_BUILD=ON

.\validate-and-promote.ps1

# Avec une installation située ailleurs :
.\validate-and-promote.ps1 -GameRoot 'D:\Jeux\SLRR'
```

### Validation avant remplacement de l'exécutable

`validate-and-promote.ps1` :

1. compile le programme dans `build-bgfx/candidate/` ;
2. construit et lance les tests avec les ressources originales ;
3. archive les sources, les empreintes SHA-256, le journal des tests et les exécutables dans `../checkpoints/` ;
4. remplace `build-bgfx/Release/slrr_host.exe` uniquement si les tests réussissent.

L'ancien exécutable est conservé dans la sauvegarde. Fermez le jeu avant cette étape : un exécutable ouvert empêche son remplacement. Une simple compilation avec `SLRR_STAGE_BUILD=ON` ne met à jour que le candidat.

Les archives ne contiennent ni les données du jeu ni les dépendances de `third_party/`.

## Lancement

```powershell
# Démarrage normal : intros, puis menus.
.\build-bgfx\Release\slrr_host.exe --game --render bgfx --root ..\game

# Accès direct au garage pour le développement.
.\build-bgfx\Release\slrr_host.exe --game --auto-new --render bgfx --root ..\game

# Vérification du chargement des données, sans session interactive.
.\build-bgfx\Release\slrr_host.exe --probe --root ..\game

# Vérification du module Pacejka.
.\build-bgfx\Release\slrr_host.exe --probe-pacejka
```

Le chemin des données peut aussi être défini avec `$env:SLRR_GAME_ROOT`. `--root` reste prioritaire. Lancez `slrr_host.exe --help` pour les autres options.

Dans le garage : clic droit + glisser pour l'orbite, flèches et molette pour la caméra. Échap permet de passer les intros. Fermez la fenêtre pour quitter.

## Tests

```powershell
cmake --build build-bgfx --config Release `
  --target host_runtime_test ui_state_test scene_instances_test --parallel
ctest --test-dir build-bgfx -C Release --output-on-failure
```

Avec les données de référence, la suite compte **12 tests CTest**. Elle couvre notamment :

- les effets simultanés, `money.wav`, les boucles et l'arrêt des voix ;
- la connexion et l'état de lecture de la sortie audio des intros ;
- Tune et les branches `switch` des scripts ;
- le HUD DAY/TIME, y compris après sa recréation ;
- l'ouverture des dialogues de carrières, leurs libellés et le défilement ;
- le remplacement des descriptions du track selector sans accumulation ;
- les états de hover, les boutons activés/désactivés et le focus ;
- les cinq paint cans grises, la page rouge suivante et leur disparition à la fermeture ;
- les transformations, l'isolation des scènes d'inventaire et les ressources de skydome.

Les tests d'intégration sont ajoutés lorsque `SLRR_TEST_GAME_ROOT` pointe vers les données du jeu. Sans ces données, seuls les tests autonomes sont disponibles. La commande de validation exige les assets pour éviter une promotion fondée sur une suite partielle.

Ces tests ne valident pas un cycle complet de chargement/enregistrement de carrière ni une session de conduite. Les vérifications audio confirment la création des voix et l'état de lecture ; une écoute reste utile pour juger le mixage. Les effets positionnels utilisent actuellement le mixage 2D.

## Organisation

```text
include/slrr/     Interfaces du host
src/jvm/          Parseur TUFA, interpréteur TREE et résolution des appels
src/natives/      Fonctions natives exposées aux scripts
src/resources/    RPK, SCX, textures, CFG et instances de scène
src/render/       Backends de rendu et polices
src/ui/           Widgets, focus, hover et formatage de l'heure
src/platform/     Entrées, audio, caméra et FMV Windows
src/boot/         Démarrage et boucle principale
src/physics/      Module Pacejka expérimental
shaders/          Shaders bgfx
tests/            Tests autonomes et intégration avec le jeu
third_party/      Dépendances locales, ignorées par Git
```

Les dossiers `game/`, `game2/`, `sources/`, `sources2/`, `repos/` et `docs/` du poste de développement sont extérieurs à ce dépôt. Ils servent de données et de références locales. [QUARRY.md](QUARRY.md) décrit ces références ; certains de ses chemins supposent cet espace de travail élargi.

Les projets [SLRR_rev](https://github.com/nedobylskiy/SLRR_rev), [SLRR-JVM-decompiler](https://github.com/nedobylskiy/SLRR-JVM-decompiler) et [io_invo_scx](https://github.com/nonamesex/io_invo_scx) servent notamment de références pour les formats et le comportement du jeu. Le rendu utilise [bgfx](https://github.com/bkaradzic/bgfx).
