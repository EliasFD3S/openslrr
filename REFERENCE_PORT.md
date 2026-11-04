# Voiture, catalogue et matériaux — 4 novembre 2026

Référence examinée : [SLRR_rev, branche codex/physics-reverse-port, c65675f](https://github.com/nedobylskiy/SLRR_rev/tree/c65675f).
Les scripts TUFA et les assets installés restent la source des classes, des pièces stock et de leurs paramètres. Le host implémente leurs opérations natives en C++.

## Assemblage

`part_config` conserve les références RPK qualifiées, les poses, les slots, les liens `attach`/`compatible`, les roues, les rendus et les pièces stock. `deformable` fournit la coque principale d'un châssis. Les rendus conditionnels et les alternatives LOD ne sont pas tous dessinés simultanément.

`part_runtime` crée la classe originale, appelle son constructeur, exécute `addStockParts(Descriptor)` et installe les pièces par les slots CFG. Les identités `compatible` forment un graphe de slots ; elles ne se confondent pas avec l'arbre des catégories RPK. Les transformations combinent les deux frames d'attachement ; les modèles SCX restent en centimètres, les CFG en mètres.

Les identifiants des instances sont distincts des identifiants de ressources. Les wrappers GameRef pointent vers l'instance canonique. Les callbacks Java `addpart`/`rempart` maintiennent la liste des pièces du châssis. La destruction retire aussi les rendus enfants.

Cas de référence : `cars/racers/Baiern.rpk:0x6`, 76 pièces et 82 instances de rendu. Le placement au sol est statique. Les cadres de roue sont les cadres CFG au repos ; le solveur de suspension et de conduite reste à intégrer.

## Catalogue

`RpkIndex::children_of` parcourt les liens de parenté qualifiés par les dépendances des RPK. Les API d'itération et de tableau partagent ce parcours sans limite arbitraire de 512 enfants. Les handles retournés exposent les requêtes GameRef sans créer une pièce.

`isScripted` consulte la classe déclarée dans le RPK et son héritage. Les pages et filtres de `Catalog` restent les scripts d'origine ; les pièces visibles sont créées à la demande par `InventoryItem_Part`, avec leurs modèles et leurs caméras d'inventaire. Les tests vérifient aussi des prix positifs. Les retours TREE flottants vers une méthode déclarée entière sont convertis à la frontière de l'appel.

Le cycle complet achat, montage interactif et sauvegarde n'est pas couvert par cette validation.

## Matériaux

Références : `src/formats/scx.ts`, `src/render/part-paint.ts`, `src/render/vehicle-material.ts` et `docs/vehicle-materials.md` de la révision ci-dessus.

Le parseur conserve le type de matériau, ses flags, les indices base/mix/reflet, leur canal et leur transformation UV, la force de réflexion et les trois jeux d'UV. En SCX v4, le type et les flags sont deux mots distincts.

La texture CFG désigne le premier lien remplaçable du modèle. `Part.setTexture` met à jour ce lien par instance sans modifier les atlas ou reconstruire la géométrie. Le shader des pièces combine la base et la couche secondaire avec son alpha, puis une contribution de l'envmap. Le type 0x15 masque cette contribution avec le complément de l'alpha secondaire. Une réflexion générée non liée, ou une surface peinte sans texture de reflet liée, utilise l'envmap globale choisie par `GfxEngine.setGlobalEnvmap`.

Cette adaptation conserve la structure des couches ; elle ne reproduit pas encore tous les shaders du jeu. Transparences, éclairage et pose de certaines pièces demandent encore comparaison au jeu original. Pas de cubemap dynamique, de composition des décalcomanies ni de simulation des lampes dans cette étape.

## Vérification

- 16 tests CTest, avec les données originales, incluant les anciens correctifs UI/audio.
- Démarrage réel `--spawn-car ... --render null --no-wait` : garage, 76 pièces, sortie normale.
- Captures GPU bgfx : voiture dans le garage et aperçus 3D de la page Body panels.
- Promotion par `validate-and-promote.ps1` avec sauvegarde de l'ancien exécutable et des sources.
