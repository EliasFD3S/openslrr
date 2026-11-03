# Carrière (`repos/`) → modules `host/`

On **ne copie pas** les arbres tels quels. On lit, on comprend, on réécrit.

## Outils script (à garder à jour)

| Repo | Rôle | Branche | Statut local (2026-10-28) |
|---|---|---|---|
| [SLRR-JVM](https://github.com/nedobylskiy/SLRR-JVM) | Interpréteur **source** `.java` (pas TUFA) | `main` only | à jour — utile pour prototypes / dialecte |
| [SLRR-JVM-compiler](https://github.com/nedobylskiy/SLRR-JVM-compiler) | `.java` → TUFA `.class` | `main` only | à jour — round-trip mods / regen classes |
| [SLRR-JVM-decompiler](https://github.com/nedobylskiy/SLRR-JVM-decompiler) | TUFA → `.java` + IR TREE | `main` only | **spec TUFA/OP_FLAGS** pour `host/src/jvm/tufa.cpp` |

```powershell
# Depuis open-SL
git -C repos/SLRR-JVM pull
git -C repos/SLRR-JVM-compiler pull
git -C repos/SLRR-JVM-decompiler pull

# Décompiler un .class stock
cd repos/SLRR-JVM-decompiler
npm ci; npm run build
node dist/src/cli.js --ir --out ../../host/out/decompiled `
  ../../game/system/Scripts/lang/System.class

# Compiler une source (jeu comme index de types)
cd ../SLRR-JVM-compiler
npm ci; npm run build
node dist/src/cli.js --root ../../game --out ../../host/out/classes --check
```

Pas d’autres branches distantes visibles aujourd’hui (seulement `main`). Rescan `git branch -a` après chaque `fetch`.

## Runtime C++ (`host/`)

| Besoin | Pioche | Destination |
|---|---|---|
| Parse TUFA + OP_FLAGS | **SLRR-JVM-decompiler** `src/tufa.ts` | `src/jvm/tufa.cpp` ✅ |
| Écrire / vérifier TUFA | SLRR-JVM-compiler `src/tufa.ts` | tests / outil plus tard |
| Dialecte source / natives DX | SLRR-JVM | référence fail-closed |
| ABI natives catalogue | `docs/native_registry.json` | `natives/` |
| TREE eval (exec) | decompiler IR + reverse TreeHost | `jvm/tree_eval.cpp` ✅ (boot subset) |
| RPK / SCX | SLRR_rev formats / FindFile | `resources/rpk_index.cpp` ✅ TOC |
| SCX INVO | [io_invo_scx](https://github.com/nonamesex/io_invo_scx) `imhex/scx.hexpat` + SLRR_rev `scx.md` | `resources/scx_mesh.cpp` ✅ summary |
| Renderer | idée Three.js → **bgfx** | WGL fallback `bgfx_backend.cpp` ; libbgfx `bgfx_lib_backend.cpp` + `shaders/*` (`-DSLRR_WITH_LIBBGFX=ON`) ✅ D3D11 |
| DDS | stock garage/frontend | `resources/dds_image.cpp` ✅ DXT1/3/5 |
| Career path | `sources/.../MainMenu.java` | `boot/section_machine.cpp` ✅ |
| Pacejka leaf SL2 | street-legal `physics.md` §9 + SLRR_rev `codex/pacejka-research` | `physics/sl2_pacejka.cpp` ✅ réf. ; docs `docs/physics/sl2-pacejka.md` |
| WheelRef 17 coef / Tyre.java | SLRR_rev `wheel-ref-native.ts` + `sources/.../Tyre.java` | doc mapping ; natives WheelRef plus tard |
| Dégâts condition / tear | SLRR_rev `deformable-physics.md` + Part wear natives | `docs/physics/damage-condition.md` |

```powershell
git -C repos/SLRR_rev fetch origin
git -C repos/SLRR_rev log --oneline origin/codex/pacejka-research -5
# Feuille double + golden :
.\host\build-bgfx\Release\slrr_host.exe --probe-pacejka
```

## Règles

1. `repos/` = lecture seule pour le produit.
2. Spec format = decompiler/compiler TS ; exécution = C++ `host/`.
3. Native manquante = erreur explicite.
4. Soft≡PE hors north-star — New Career→Garage tient (host leaves) ; Soft≡PE reste hors produit.
5. **Multiplayer hors plan** — aucune phase réseau / authoritative server.
6. Physique véhicule = **après** hub garage ; feuille Pacejka SL2 en réserve (`--probe-pacejka`),
   pas encore le drive loop. Refs génériques : rsms / Rem Voorhuis ; oracle native = street-legal.
