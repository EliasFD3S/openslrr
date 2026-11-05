# Audit physique SLRR_rev — 5 novembre 2026

## Périmètre et méthode

Audit du dépôt public [nedobylskiy/SLRR_rev](https://github.com/nedobylskiy/SLRR_rev), directement sur GitHub, sans utiliser le clone local. Les neuf branches, leurs arbres complets et les différences d'historique des deux branches récentes ont été consultés. Les identités de blobs permettent de distinguer un nom de branche plus récent d'un vrai changement de physique.

Inventaires, réponses GitHub et sources épinglées sont conservés dans `out/slrr-rev-audit-20261105/` (ignoré par Git). Les sources du host et le README préexistant n'ont pas été modifiés. Aucun jeu, test ou build n'a été lancé pour cet audit ; les tests amont mentionnés ci-dessous ont été lus, pas exécutés.

## Branches vérifiées

| Branche | HEAD consulté | Intérêt pour le port |
| --- | --- | --- |
| `main` | `cef874dfb54f8f59ddbd0059702d97210ab9f97a` | Ancien socle avec corps rigide et suspensions 3D ; pas la meilleure base pour les dernières natives et liaisons |
| `codex/physics-reverse-port` | `c65675fe2e67f316a8bca6ce010bf34a00416c52` | Calculs récupérés : pneus, glissement, charge, ressort/amortisseur, embrayage, freinage, thermique ; documentation des unités et approximations |
| `codex/suspension-slot-linker` | `a2ee10def17b8838184651ca0b8d174d198e5aed` | Base recommandée : hérite du port physique et ajoute les derniers contrats de slots/linked, carriers et géométrie des suspensions |
| `codex/graphics-debug` | `d51e77de77e10c05ceb8d427c30b0247cbd81837` | Descend de suspension-slot-linker ; les modules physiques inventoriés sont identiques, pas de nouveau solveur à récupérer ici |
| `codex/pacejka-research` | `f16d7499d1bd82203f885367a8eaf2e2ccd170f6` | Recherche historique du pneu ; ne pas remplacer les calculs natifs plus récents par ce seul prototype |
| `codex/wasm-soft-body` | `cf4aad20046eb29b224a90eac8d1a7f86df964a1` | Backend C++/WASM des panneaux déformables, distinct du châssis et des contacts de suspension |
| `codex/garage-run` | `12e8564bd5c40e858c309e81865e213abdcefce6` | Intégration garage plus ancienne, PhysicsRef et collisions ; pas le dernier modèle de suspension |
| `codex/java-init-entry` | `8934ba41fa7ce0f0c8ae802a3b1e6799958ec0ff` | Entrée Java et ancien socle de physique |
| `codex/pre-major-change-snapshot` | `1d48b60efdee7c1bce3aa552943b7d9d7666b073` | Sauvegarde historique, pas une base supplémentaire à mélanger |

Comparaisons GitHub : suspension-slot-linker est 18 commits devant physics-reverse-port, sans divergence ; graphics-debug est 13 commits devant suspension-slot-linker, sans divergence. Son dernier commit consulté date du 5 novembre à 14:01:52, heure de Paris. Les principales sources et les tests physiques ont les mêmes blobs sur ces deux dernières branches.

## La simulation dynamique réellement présente

Chemin principal à porter, dans la révision `a2ee10de` :

1. `Vehicle3D` crée un `RigidBody3D` avec la masse et le centre de masse calculés depuis les `body` des pièces installées.
2. `FixedClock(240, 8)` fournit un pas fixe de 1/240 s, avec au plus huit pas par appel. La dette temporelle est conservée et signalée, pas supprimée silencieusement.
3. `Suspension3D.step` recherche les contacts au sol depuis les points d'ancrage, calcule les longueurs des ressorts, les forces et l'amortissement. Les paramètres viennent notamment des WheelRef exécutées par les scripts.
4. Les forces appliquées aux ancrages créent forces ET couples sur le corps rigide. La transmission et les forces de pneus contribuent au même état.
5. `RigidBody3D.integrate` intègre vitesse, position, moment cinétique et orientation quaternion, avec gravité. L'inertie est une approximation de boîte.
6. Les contacts de châssis sont résolus avec impulsions, friction et correction limitée de pénétration ; `Vehicle3D` fait quatre itérations de contact. Le décor fournit aussi les collisions latérales.
7. Le résultat écrit la matrice de la racine ; `Suspension3D.sync` reporte débattement, braquage et rotation des roues vers les pièces et leurs rendus.

C'est une vraie simulation dynamique : elle peut chuter, se comprimer, décoller et basculer. Ce n'est pas une simple écriture de hauteur, et aucun moteur Bullet/Jolt n'est une dépendance de ce chemin amont.

## Ce que « jointures » recouvre ici

Deux mécanismes distincts ne doivent pas être confondus :

- **Slots et rendus liés** : `linked-slots.ts`, `linked-render.ts`, `linked-suspension.ts` résolvent les ancrages, leurs orientations, le groupe hérité du montage racine, le filtre de slot parent et la première règle applicable. La recherche se fait sur la pièce puis ses ancêtres. Les carriers vivants transmettent débattement et braquage ; les bras peuvent suivre le débattement sans hériter du braquage du moyeu.
- **Dynamique de suspension** : `Suspension3D` utilise des contacts par rayons et une approximation de carrier. Ce n'est pas une chaîne multi-corps complète où chaque bras/moyeu est un corps indépendant relié par des contraintes physiques natives.

Les auteurs l'indiquent explicitement dans `docs/physics-reverse-port.md` et `docs/suspension-slot-linker-experiment.md` : le solveur natif multi-corps/contact, les trois nœuds physiques WheelRef et certaines géométries arm/hub/instant-center ne sont pas entièrement portés. `setArm`, `setHub` et `setInstantCenter` conservent des données ; leur présence ne prouve pas leur utilisation par un solveur de jointures complet.

Le C++ de `native/soft-body/solver.cpp` est un solveur de nœuds/poutres, attaches et contacts pour les panneaux déformables. Il ne remplace pas `RigidBody3D` + `Suspension3D` pour poser un véhicule sur ses roues. Les pièces détachées et panneaux déformables échangent des impulsions avec le châssis via `Vehicle3D` ; les porter constitue une étape distincte.

## Adaptations amont à conserver comme telles

Complément côté host : la référence [EliasFD3S/slrr-reverse](https://github.com/EliasFD3S/slrr-reverse/tree/5c42f3b139aef4c7b9e0b6420c5704e22c0e6cd7) et la lecture du binaire original ont permis d'identifier la géométrie de pose native à `0x44FA70`. Le host porte désormais la projection du bras et les modes de moyeu `0`/`-1`, avec des repères bras/moyeu/roue distincts. Ce complément dépasse le stockage de paramètres constaté dans les branches SLRR_rev auditées ; il ne fournit pas encore le solveur de forces multi-corps complet. Voir [NATIVE_SUSPENSION_GEOMETRY.md](NATIVE_SUSPENSION_GEOMETRY.md).

- L'inertie de châssis issue de sa boîte, l'inertie de roue de 1,6, les conversions d'inertie moteur/transmission, les limites de forces et butées de sécurité appartiennent à l'adaptateur amont. Ce ne sont pas des constantes natives prouvées du jeu.
- La branche utilise aussi des corrections de placement mesurées : hauteur de frame `+0,165 m` et décalages latéraux FL/FR/RL/RR de `+0,055`, `-0,055`, `-0,075`, `+0,075 m`. Le host ne reprend pas ces calibrations : la capture des dimensions de la Duhen dans le jeu original confirme les centres CFG `wheel` (voies 1366/1320 mm), sans addition des offsets de slots de montage. L'ancrage vertical utilise `base + travel` du CFG, indépendamment de la longueur de repos réglée.
- Les ancrages CFG et les états des natives sont les entrées. Le terrain et les corps utilisent les mètres de l'espace script ; la conversion SCX et la réflexion Z restent à la frontière rendu/ressources.
- La fidélité exacte à la conduite du binaire original n'est pas établie par le simple port de ces fichiers.

## Écart avec le host actuel

| Élément | État observé dans le host | Travail nécessaire |
| --- | --- | --- |
| CFG, masse, centre de masse | Présents dans `part_config` / `part_runtime` | Fournir ces données au corps rigide dynamique |
| WheelRef | Stockage force, amortissement, longueurs, rayon, pneus, arm/hub/IC | Connecter l'état au pas physique, pas seulement le conserver |
| Ressorts au repos | `settle_parked` résout une longueur statique | Conserver comme diagnostic/initialisation, ne pas substituer à l'intégration dynamique |
| Calage sol | `part_plant_parked` déplace directement la caisse depuis les plus bas meshes | Supprimer son autorité sur les voitures simulées ; seul le solveur doit écrire la pose |
| Terrain | Triangles préchargés et dynamiques, recherche de hauteur | Fournir contacts, normales et absence de contact explicite, en espace script, sans plan implicite y=0 |
| Corps rigide | Pas de boucle dynamique véhicule reliée au rendu | Porter position/COM, quaternion, vitesses, inertie, forces, couples et impulsions |
| Horloge | Affichage appelle le calage par image | Ajouter un accumulateur à pas fixe, indépendant du framerate |
| Slots/linked | Résolution et placement déjà partiellement portés | Comparer les règles et injecter les carriers calculés à chaque pas, sans dérive cumulative |
| Dyno | Module C++ et `forceUpdate` déjà présents | Réutiliser ses courbes dans la transmission après validation de la chute et de l'appui |

## Ordre de port et critères d'acceptation

1. Corps rigide 6 DOF et horloge fixe, sans JVM ni rendu dans le noyau : chute libre vérifiée, impulsion excentrée faisant tourner le corps, inertie et transformations COM cohérentes.
2. Terrain + quatre ressorts/amortisseurs alimentés par les données CFG/WheelRef. Lâcher la caisse au-dessus du garage ; observer une phase en l'air, le contact, la compression et la stabilisation. Pas de déplacement artificiel par `plant`, ni de ressort qui tire le véhicule vers le sol sans contact.
3. Écrire le résultat dans la racine rendue et propager le débattement aux familles de frames 101/201/301, puis au linker ordonné. Tester changement de longueur, braquage et remplacement de pièce sans décalage cumulatif.
4. Vérifier sur sol plat, sol incliné, fosse du garage, sol sous zéro, absence de sol, impulsion sur un seul coin et cadences d'affichage différentes. À l'équilibre, la somme des forces de support doit être cohérente avec la masse * g.
5. Pneus et transmission : porter les kernels récupérés de `native-vehicle-physics.ts`, leurs états de glissement et leurs tests numériques, puis `MechanicalSystem` en gardant explicites ses conversions d'unités.
6. Pièces détachées et déformation : port séparé du couplage `Vehicle3D` / XPBD, avec conservation des vitesses et impulsions au détachement.

Pour reproduire une suspension native articulée au-delà de l'approximation SLRR_rev, il faudra compléter la recherche des contraintes et des nœuds WheelRef ; aucune des neuf branches consultées ne fournit à elle seule ce port natif complet.

## Sources principales épinglées

- [RigidBody3D](https://github.com/nedobylskiy/SLRR_rev/blob/a2ee10def17b8838184651ca0b8d174d198e5aed/src/engine/rigid-body-3d.ts)
- [Suspension3D](https://github.com/nedobylskiy/SLRR_rev/blob/a2ee10def17b8838184651ca0b8d174d198e5aed/src/engine/suspension-3d.ts)
- [Vehicle3D](https://github.com/nedobylskiy/SLRR_rev/blob/a2ee10def17b8838184651ca0b8d174d198e5aed/src/engine/vehicle-3d.ts)
- [Contrats du linker](https://github.com/nedobylskiy/SLRR_rev/blob/a2ee10def17b8838184651ca0b8d174d198e5aed/docs/suspension-slot-linker-experiment.md)
- [Calculs récupérés et limites du port natif](https://github.com/nedobylskiy/SLRR_rev/blob/a2ee10def17b8838184651ca0b8d174d198e5aed/docs/physics-reverse-port.md)
