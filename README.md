# slrr_host

Host C++ propre pour *Street Legal Racing: Redline*.

- **Produit** : ce dossier (`host/`)
- **Données** : `../game`, `../sources`
- **Carrière / RE** : `../repos` et `../docs` — on y pioche, on n’y commit pas le runtime

## Principe

```text
scripts TUFA  →  Jvm  →  NativeRegistry  →  world / I/O / phys
                                      └→  IRenderBackend (null → bgfx)
```

Même idée que SLRR_rev + Three.js : le renderer est un **backend**, pas le moteur.

## Build (MSVC Win32 recommandé)

```powershell
cd host
# WGL interim (pas de libbgfx)
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DSLRR_WITH_LIBBGFX=OFF
cmake --build build --config Release

# libbgfx + shaders mesh (C++20, shaderc)
cmake -S . -B build-bgfx -G "Visual Studio 17 2022" -A Win32 -DSLRR_WITH_LIBBGFX=ON
cmake --build build-bgfx --config Release
```

## Run

```powershell
.\host\build\Release\slrr_host.exe --probe
.\host\build\Release\slrr_host.exe --game --auto-new --no-wait
.\host\build-bgfx\Release\slrr_host.exe --game --auto-new --render bgfx --no-wait
.\host\build-bgfx\Release\slrr_host.exe --game --auto-new --auto-cmd mainmenu --render bgfx --no-wait
```

Ou : `$env:SLRR_GAME_ROOT = "...\open-SL\game"`

## Feuille de route

### Fait (A–D2 · hub + render GDI)

1. ~~TUFA + TREE leaf + Splash.osdCommand~~
2. ~~Natives demand-driven + rpkScan + `--auto-new` → Garage~~
3. ~~Splash/MainMenu `enter`/`exit` TREE~~
4. ~~CMD_NEW via `MainMenuDialog.osdCommand(50)`~~
5. ~~Garage.enter + createOSDObjects (15 boutons, CMD_* stock)~~
6. ~~RPK TOC + ResourceRef.load (alias / texture / sourcefile)~~
7. ~~Garage.osdCommand host leaf~~ (`--auto-cmd mainmenu|street`)
8. ~~SCX INVO parse~~ ([io_invo_scx](https://github.com/nonamesex/io_invo_scx) + SLRR_rev) → bbox/verts
9. ~~Viewport garage~~ SCX → WGL/OpenGL draw (`--render bgfx`)
10. ~~SCX geometry upload~~ positions/normals/indices (io_invo_scx)
11. ~~DDS DXT~~ — textures co-located SCX (`g1/*.dds`) WGL + libbgfx
12. ~~libbgfx~~ — `-DSLRR_WITH_LIBBGFX=ON` + shaders dxbc/glsl → D3D11 submit
13. ~~Caméra garage~~ — orbite stock (`cameraSetup` dist/look/FOV) + RMB/flèches/molette

### Next (monde)

14. OSD cliquable · HitStreet → Valocity · SDAT · Physique · **Multiplayer hors plan**

Contrôles viewport garage : **clic droit + glisser** orbite · **flèches** · **molette** zoom · fermer la fenêtre pour quitter.

Politique TREE : &lt;~200 nœuds + natives OK → TREE ; &gt;~500 ou natives critiques → host leaf fidèle au `.java`.

### Physique (réf. quarry, pas encore le solver de conduite)

- Feuille Pacejka SL2 : [`docs/physics/sl2-pacejka.md`](../docs/physics/sl2-pacejka.md) + `host/src/physics/sl2_pacejka.cpp`  
  `slrr_host --probe-pacejka`
- Condition / détachements : [`docs/physics/damage-condition.md`](../docs/physics/damage-condition.md)  
  (SLRR_rev Baiern + Wear/Tear natives)
- Approche générique : [rsms — car-physics](https://rsms.me/etc/car-physics/), [Rem Voorhuis](https://medium.com/@remvoorhuis/how-to-program-realistic-vehicle-physics-for-realtime-environments-games-part-i-simple-b4c2375dc7fa)

Outils : garder `repos/SLRR-JVM{,-compiler,-decompiler}` et `repos/SLRR_rev` (`codex/pacejka-research`) à jour. Voir [QUARRY.md](QUARRY.md).
