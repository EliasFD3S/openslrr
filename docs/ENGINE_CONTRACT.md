# OpenSLRR — contrat moteur et guide d’architecture pour les agents

État documentaire : 11 novembre 2026, branche `enhanced`.

Ce document explique comment travailler sur le moteur sans casser les scripts,
les ressources et les mods. Il décrit l’implémentation présente dans ce dépôt ;
il ne constitue pas une spécification exhaustive de l’exécutable original.
Les fichiers et symboles cités sont les points d’entrée pour vérifier chaque
contrat. Les valeurs numériques peuvent évoluer : relire le code avant de les
modifier ou de les présenter comme une garantie.

## Sommaire

1. [Objectif et périmètre](#1-objectif-et-périmètre)
2. [Contrat de compatibilité](#2-contrat-de-compatibilité)
3. [Carte du dépôt](#3-carte-du-dépôt)
4. [Démarrage et boucle de jeu](#4-démarrage-et-boucle-de-jeu)
5. [VM TUFA/TREE et natives](#5-vm-tufatree-et-natives)
6. [Threads, événements et dialogues](#6-threads-événements-et-dialogues)
7. [Mémoire et durée de vie](#7-mémoire-et-durée-de-vie)
8. [Ressources et identifiants](#8-ressources-et-identifiants)
9. [Scène, cartes et objets natifs](#9-scène-cartes-et-objets-natifs)
10. [Voitures, CFG et suspensions](#10-voitures-cfg-et-suspensions)
11. [Physique et dégâts](#11-physique-et-dégâts)
12. [Rendu Enhanced](#12-rendu-enhanced)
13. [Interface, entrées et options](#13-interface-entrées-et-options)
14. [Audio et vidéo](#14-audio-et-vidéo)
15. [Sauvegarde et transitions](#15-sauvegarde-et-transitions)
16. [Méthode de travail et validation](#16-méthode-de-travail-et-validation)
17. [Diagnostic par symptôme](#17-diagnostic-par-symptôme)
18. [Limites et références](#18-limites-et-références)

## 1. Objectif et périmètre

OpenSLRR réimplémente en C++20 le moteur nécessaire pour exécuter les scripts
et les données de Street Legal Racing: Redline. La cible actuelle est Windows
x64, avec bgfx pour le rendu. Les données du jeu ne sont pas distribuées ici.
La référence fonctionnelle principale est SLRR 2.2.1 MWM.

Les scripts contrôlent une grande partie du jeu : menus, sections, catalogue,
assemblage, réglages et carrière. Le host fournit l’exécution, les natives,
les ressources, l’interface, la simulation et les services de plateforme.

`enhanced` autorise un rendu modernisé : ciel procédural, éclairage, ombres
et réflexions dynamiques. Cette liberté visuelle ne supprime pas les contrats
de durée de vie, de données et d’appels attendus par les scripts.

## 2. Contrat de compatibilité

### Les scripts sont des clients du moteur

Une fonctionnalité présente dans MWM peut être entièrement réalisée par les
scripts, sans modification de l’exécutable original. Avant d’ajouter une
exception `if (class_name == ...)` dans la VM, suivre le script et identifier
la primitive générale mal interprétée ou manquante.

La VM ne doit pas connaître une voiture particulière, le CarSelector MWM,
le nom d’un menu ou une étape spécifique de carrière pour exécuter correctement
une affectation, un appel, une attente ou une destruction.

Un correctif doit normalement se placer dans la couche propriétaire :

| Problème | Couche à examiner d’abord |
| --- | --- |
| Mauvais récepteur d’affectation ou opcode absent | TREE/JVM |
| Signature native introuvable | Registre des natives, héritage, descripteur |
| Texture liée au mauvais pack | Résolution RPK/SDAT/CFG |
| Pièce mal placée | Frames de slots et géométrie de suspension |
| Son ou événement joué deux fois | Producteurs d’événements et chemins de secours |
| Ressource restant à l’écran après destruction | Durée de vie scène/UI/GPU |
| Fenêtre qui reprend une ancienne résolution | Options, Config, application du mode |

### Modifications Java

La règle de compatibilité habituelle est de faire fonctionner les scripts
stock. Des changements Java ont été autorisés pour des fonctionnalités
précises, notamment le catalogue et OptionsDialog. Cette autorisation ne
signifie pas que tout bug VM peut être contourné dans les scripts.

Pour un changement Java livré : conserver la trace des anciennes lignes en
commentaire lorsque demandé, expliquer le motif, compiler dans l’arborescence
du jeu appropriée et distribuer les fichiers nécessaires dans `../patch/`.
Un fichier présent seulement dans `../game/` n’est pas un patch reproductible.
Vérifier séparément le statut Git de ces dossiers : le dépôt principal de
travail est `host/`.

### Ne pas confondre les preuves

Une déclaration native prouve une API ; un appel stock révèle ses attentes ;
un désassemblage peut préciser son comportement ; un port tiers propose une
implémentation. Un nom de fonction ressemblant à celui de l’original ne prouve
pas une fidélité comportementale. Documenter les approximations.

## 3. Carte du dépôt

Tous les chemins suivants sont relatifs à `host/`.

| Répertoire / fichier | Rôle |
| --- | --- |
| `src/main.cpp` | Arguments, sélection du démarrage et diagnostics |
| `src/boot/boot.cpp` | Assemblage des services et amorçage du jeu |
| `src/boot/mainloop.cpp` | Mise à jour du host, entrées, événements, caméra |
| `src/boot/section_machine.cpp` | État actif et transitions des sections |
| `src/boot/script_events.cpp` | Notifications et callbacks GameType |
| `src/jvm/` | TUFA, interprétation TREE, heap, GC, fibers |
| `src/natives/` | Adaptation des APIs script vers les services C++ |
| `src/resources/rpk_index.cpp` | Packs, dépendances et résolution des ressources |
| `src/resources/gameref.cpp` | Graphe des instances et opérations GameRef |
| `src/resources/scene_instances.cpp` | Collecte scène, cartes, placements et requêtes de collision |
| `src/resources/part_config.cpp` | Lecture CFG et frames locales |
| `src/resources/part_runtime.cpp` | Instances de pièces, assemblage et liaison avec la physique |
| `src/physics/` | Dynamique véhicule, pneus, suspension, dyno et déformation |
| `src/render/` | Backend bgfx, éclairage, ombres, OSD et debug ImGui |
| `shaders/` | Sources shaders compilées par le build |
| `src/ui/` | Registres et comportement de l’OSD |
| `src/platform/` | Entrées, options, curseur, effets, musique et FMV |
| `include/slrr/` | Structures et interfaces entre ces systèmes |
| `tests/` | Régressions ciblées arraycopy, PTX, dyno/GC |
| `out/` | Produits de compilation et diagnostics locaux ; pas une API stable |

Les dossiers voisins `../game/`, `../sources/`, `../sources2/` et `../repos/`
peuvent servir de données ou de références selon le poste. Leur présence ne
doit pas être présumée dans un clone neuf.

## 4. Démarrage et boucle de jeu

Le chemin de démarrage part de `main.cpp`, configure les services, ouvre les
ressources et charge les classes avant l’exécution du jeu. Lire `boot.cpp`
pour l’ordre exact et les chemins de diagnostic.

Schéma conceptuel, sans représenter chaque appel intermédiaire :

```text
Arguments / racine du jeu
        ↓
Services plateforme + RpkIndex + NativeRegistry + Jvm
        ↓
Classes TUFA → Init / GameLogic → section active
        ↓
Pompe host ↔ scripts coopératifs ↔ événements / entrées
        ↓
État pièces et simulation → collecte scène → bgfx → présentation
```

Dans `mainloop_frame`, on trouve notamment la musique, la mise à jour des
options, le temps du ciel, le scheduler script, l’adoption de section, les
messages, le contrôleur, les hotkeys, les callbacks, la caméra et les timers.
Cet ordre a des conséquences : ne pas déplacer arbitrairement une livraison
d’événement ou ajouter une seconde pompe d’un même port.

Les présentations demandées depuis un chargement ou un dialogue peuvent
imbriquer une pompe host. Toute fonction appelée dans ce contexte doit être
examinée pour la réentrance : callbacks, collecte mémoire et transitions
ne doivent pas s’exécuter deux fois sur un état partiellement modifié.

## 5. VM TUFA/TREE et natives

### Ce que signifie « Java » ici

Ce host n’embarque pas une JVM Java SE classique. Il charge les classes TUFA
du jeu et interprète leurs programmes TREE. Ne pas appliquer automatiquement
les conventions d’un fichier `.class` JVM standard ou du bytecode Java SE.

Points d’entrée : `tufa.cpp`, `tree_eval.cpp`, `jvm.cpp`, `native_registry.cpp`.
`types.hpp` définit `Value` avec les tags `Void`, `Int`, `Float`, `Obj`.
Les entiers script sont des `int32_t`, même si le processus est x64.

`Object` porte notamment le nom de classe, les champs, le contenu des strings,
et les slots des tableaux. L’identité d’objet n’est pas son contenu.
Les tableaux ont `is_array`, `slots` et un champ script `length` à maintenir.

### Résolution et signatures

Une native est identifiée par classe, nom et descripteur. Vérifier aussi les
surcharges, le dispatch hérité et le récepteur. Une native enregistrée avec
un mauvais descripteur peut sembler absente alors que son corps existe.

Pour une erreur `invoke_by_name miss` : relever la classe réelle, le nom,
les arguments et les déclarations disponibles avant d’ajouter une méthode.
Pour `unsupported op` : examiner la sémantique TREE et ses appelants ; ne pas
remplacer l’instruction par un retour de succès silencieux.

Les statics appartiennent à l’état de classe de la VM. Ne pas remplacer un
champ static manquant par une constante locale à un écran. Vérifier le
chargement et l’initialisation de classe, les accès hérités et les références
de ressources contenues dans ces champs.

### Conventions propres au host

`System.arraycopy` conserve actuellement un comportement d’agrandissement
automatique de destination utilisé par le host. Le helper `native_arraycopy`
valide les plages et les débordements, conserve les copies chevauchantes et
met à jour `length`. Ne pas le « normaliser » selon Java SE sans analyser
les scripts et les régressions existantes.

Une native avec un corps n’est pas nécessairement complète. Distinguer dans
les audits : absente, no-op explicite, valeur fixe/mock, partielle, implémentée,
et validée dans un scénario défini. Voir `NATIVE_AUDIT.md`.

## 6. Threads, événements et dialogues

Les threads script sont coopératifs, réalisés avec des fibers Windows. Ils
conservent leur pile d’interprétation et d’appels C++ pendant un sleep/wait.
Ils ne sont pas des workers Java parallèles. La VM reste sur le thread host.

`start` programme l’exécution ; `sleep` suspend avec une échéance ; `wait`
attend une notification ; `stop` doit permettre le déroulement des scopes
avant de libérer la pile. Du travail C++ long dans une native ne devient pas
préemptible simplement parce que l’appelant est une fiber.

Les notifications GameType et les callbacks sont des registres distincts.
Les modes documentés 8, 9 et 28 concernent `control(float)`, `drive()` et
`animate()`. L’itération doit tolérer une désinscription ou réinscription
pendant le callback. Voir `SCRIPT_RUNTIME.md` pour les flags et budgets.

Le host conserve certains chemins de secours pour des scripts non actifs.
Ils doivent s’effacer lorsqu’un thread script consomme déjà le port ou produit
la même action. Sinon : double clic logique, hover répété, son en boucle ou
fermeture prématurée de dialogue.

Contrat modal :

- La fenêtre active possède son bouclier modal et sa fin de vie.
- Un clic sur un slider doit livrer l’événement de réglage au bon objet.
- Un réveil de MessagePort ne signifie pas que le dialogue est terminé.
- Les événements ne doivent pas traverser le dialogue vers la voiture.
- La fermeture d’un dialogue ne doit pas retirer le bouclier d’un autre.

## 7. Mémoire et durée de vie

`Jvm` possède le heap géré. Le GC est mark/sweep et non déplaçant. Il ne
remplace pas les destructeurs de ressources, les invalidations de cache ou
la libération GPU.

Un `Object*` stocké dans une structure C++ n’est pas une racine automatique.
Un système qui retient réellement un objet doit exposer ses racines. Les
locaux devant survivre à un yield nécessitent notamment `Jvm::RootScope`
ou une protection équivalente déjà fournie par leur chemin d’appel.

Le choix fort/faible fait partie du contrat :

| Cas | Propriété attendue |
| --- | --- |
| Section active, thread vivant, callback enregistré | Maintien explicite de l’objet nécessaire |
| Local suspendu dans une fiber | Racine de pile conservée |
| Cache dérivé indexé par objet | Ne doit pas rendre l’objet immortel sans raison |
| Destruction d’une ressource | Libération des registres, hooks et données associées |
| Réinitialisation du monde | Aucun payload de l’ancien monde encore utilisable |

Le banc moteur illustre une rétention faible : `g_sessions` ne maintient plus
tous ses DynoData vivants. Le calcul temporairement actif est protégé ;
`dyno_forget` retire une session et `dyno_clear_sessions` nettoie la
réinitialisation. Voir `dyno_model.cpp` et les tests dédiés.

`System.gc()` demande une collecte différée ; les safe points empêchent la
collecte au milieu d’une mutation non sûre. `disableGC`/`enableGC` sont des
protections idempotentes, pas un compteur de références. Les finalizers et
la résurrection imposent un second traçage. Voir `VM_GC.md`.

Attention : graphe Java, graphe de ressources, registres de pièces, cache de
géométrie CPU et handles GPU ont des propriétaires différents. Une fuite FPS
après défilement de véhicules ne se corrige pas forcément avec `System.gc()`.

## 8. Ressources et identifiants

### Trois espaces à distinguer

1. Identifiant local dans un pack.
2. Référence sérialisée dans le payload d’un pack, utilisant ses dépendances.
3. Identifiant runtime du host : `(pack_id << 16) | (local & 0xffff)`.

Le numéro de dépendance sérialisé n’est pas le numéro de pack attribué par
le host. Utiliser `RpkIndex::resolve_embedded_id(owner_pack, rid)` pour les
références embarquées. Le pack propriétaire est essentiel : résoudre contre
le pack de l’appelant peut produire une ressource valide mais incorrecte.

Le passage x64 ne change pas les formats de fichiers 32 bits. Ne jamais
sérialiser un pointeur natif dans un champ resource ID ou un entier script.
Préserver les conversions signées/non signées : le bit haut d’un RID peut
être utilisé alors qu’un argument script est représenté comme `int32_t`.

### Chaîne de chargement

```text
RPK / dépendances → entrée et payload RSD → ressource de rendu / mesh
                                              ↓
                             SCX + liste de textures dans l’ordre RSD
                                              ↓
                             DDS / PTX → géométrie et matériaux CPU → GPU
```

L’ordre des clés `texture` répétées est significatif. Ne pas les mettre dans
un conteneur qui perd l’ordre ou dédupliquer aveuglément les slots.
Une texture introuvable n’est pas toujours un problème de décodeur : vérifier
d’abord owner pack, dépendance, slot, chemin et matériau.

Les PTX contiennent des données JPEG RGB et éventuellement alpha, avec des
mip levels. Les tailles et offsets doivent être bornés avant toute lecture,
y compris les deux octets des marqueurs JPEG. Les contrôles arithmétiques
ne doivent pas dépendre d’un wraparound 32 bits. Voir `ptx_image.cpp`.

## 9. Scène, cartes et objets natifs

`ResState` et `GameRefState` représentent des instances, leurs identités,
parents et enfants. Une ressource de type n’est pas son instance vivante.
L’alias script et les wrappers de référence ne justifient pas la duplication
de l’objet sous-jacent.

`scene_instances_collect` produit les instances à rendre du monde sélectionné.
Une géométrie peut être partagée ; la pose, la couleur et les remplacements
de textures sont des propriétés d’instance. Éviter de modifier une géométrie
partagée pour corriger une seule voiture.

Une carte n’est pas forcément un SCX unique. `GroundMapData` distingue les
meshes de rendu/collision, les packs extérieurs et les placements. Valo City
nécessite de résoudre les chaînes de cartes et leurs objets placés.

« Native object » dans une carte désigne ici des objets du monde issus des
descriptions/placements du moteur. Cela ne signifie pas nécessairement une
nouvelle fonction native Java à enregistrer. Examiner d’abord les données
et le chemin d’instanciation.

Séparer les décisions suivantes : chargement, résidence CPU, upload GPU,
visibilité/frustum, participation aux ombres et collision. Un objet hors
écran peut projeter une ombre visible. Un culling de rendu ne doit pas faire
disparaître le sol physique. Une stratégie future Hi-Z ne remplace pas la
résolution des placements manquants.

Les requêtes de sol, picking et sweep utilisent l’index de scène. Le plancher
préchargé du garage a une durée de vie spécifique et ne doit pas servir de
sol au dealer ou à une piste. Les révisions de sol servent à invalider les
données dérivées sans reconstruire tout l’index à chaque frame.

## 10. Voitures, CFG et suspensions

### Unités et repères

| Donnée | Convention à préserver |
| --- | --- |
| Physique et positions script/CFG | Mètres, kilogrammes, secondes |
| Poses CFG | Rotation yaw/pitch/roll en radians dans `PartPose` |
| Géométrie SCX source | Conversion d’échelle gérée par le chargement ; source en centimètres |
| Matrices de `SceneInstance` | Column-major, avec réflexion Z pour le rendu |
| Calcul physique | Repère script, sans injecter la réflexion du renderer |

Ne pas appliquer une seconde conversion d’échelle ou un second retournement Z
dans le solveur. Pour un signe de braquage ou de rotation incorrect, suivre
le repère depuis l’entrée jusqu’au hub et au mesh avant de multiplier
arbitrairement l’input par -1.

### Graphe d’assemblage

Les scripts choisissent les pièces et appellent les natives. Les CFG décrivent
les slots, poses, corps, roues et règles de rendu. `part_runtime.cpp` relie
les objets vivants à cette description et à la simulation.

Les règles `attach`/`compatible`, les IDs de slots et les indices de roue ont
des rôles distincts. Les associations de suspension à leur roue ne doivent
pas être déduites du seul ordre des meshes.

Une règle `linked a [b] [parentSlot]` peut être filtrée par slot parent.
La première règle correspondante compte. Ne pas trier ces règles. Les
calculs des moyeux, bras et frames de slots affectent à la fois la largeur
de voie, les axes de rotation et la pose visuelle.

Les renders de niveaux LOD sont des alternatives. Les dessiner tous comme
des couches produit des doublons. Un render peut appartenir à une roue
plutôt qu’à la carrosserie ; préserver cette association.

### États dérivés

Une suspension installée peut déformer/étirer son rendu pour joindre ses
ancrages. À son retrait vers l’inventaire, la pièce doit retrouver sa pose
de repos ; au remontage, recalculer depuis la nouvelle configuration.
Ne pas confondre cette adaptation géométrique avec les dégâts persistants.

Les ouvrants utilisent les pivots/axes et bornes du CFG, avec interpolation.
Ils doivent suivre les transformations de la voiture et les changements
d’assemblage, sans déplacer le repère physique principal.

## 11. Physique et dégâts

`VehicleDynamics` possède le corps rigide, les états de roues, les paramètres
de suspension et la transmission. `part_runtime` extrait les propriétés
des pièces et publie les résultats dans le graphe de rendu.

La fréquence déclarée actuellement est **120 Hz**, avec une fréquence
d’update déclarée de **60 Hz** dans `vehicle_dynamics.hpp`. `advance(seconds)`
utilise un accumulateur. Ne pas réintroduire une intégration « une fois par
image » ou supposer que l’ancienne discussion de 700 Hz décrit le code actuel.
Le gain de FPS ne doit pas accélérer le temps simulé.

Un contact terrain absent signifie de l’air, pas un sol implicite à Y=0.
Le rayon des pneus, les centres/hubs, les ressorts, amortissements, masses
non suspendues et butées doivent rester cohérents. Ne pas fixer une voiture
à une hauteur arbitraire pour masquer un problème de collision.

Pour la conduite : suivre throttle → couple dyno → embrayage/rapports →
distribution aux roues → vitesse de rotation → glissement → forces pneus
→ forces et moments sur le corps. Une perte de grip n’est pas une preuve
qu’il manque uniquement un différentiel ou une barre stabilisatrice.

Le code actif des pneus se trouve dans `tire_model.cpp` et son intégration
dans `vehicle_dynamics.cpp`. La présence de `sl2_pacejka.cpp` dans le dépôt
ne signifie pas que toute la conduite utilise cet ancien modèle.

Pour le banc moteur, une nouvelle configuration doit reconstruire les banques
de courbes correspondantes, dont la nitro. Ne pas mélanger une ancienne banque
nitro avec un nouveau pas RPM ou une nouvelle géométrie. Borner les indices
avant conversion entière ; le dernier échantillon exact doit être accessible.

La déformation et les pièces détachées demandent une cohérence entre impact,
repère local, mesh déformé, état de pièce et collision. Vérifier séparément :
vibration au repos, déformation permanente, ouverture d’un panneau et rupture
d’attache. Un filtre visuel ne corrige pas un solveur instable.

## 12. Rendu Enhanced

Le produit actuel utilise bgfx ; `--render-bgfx` n’est pas requis. Des enums,
fichiers ou commentaires historiques mentionnant D3D9/WGL ne constituent pas
la description du chemin de production. Lire CMake et `bgfx_lib_backend`.

Responsabilités principales :

| Fichier | Responsabilité |
| --- | --- |
| `bgfx_lib_backend.cpp/.hpp` | Backend, ressources GPU et orchestration |
| `bgfx_lighting.cpp` | Éclairage, ciel et réflexions selon les chemins présents |
| `bgfx_shadows.cpp` | Ressources et passes d’ombre |
| `bgfx_osd.cpp` | Dessin de l’interface |
| `debug_overlay.cpp` | Inspection et édition via ImGui |
| `scene_sun.cpp` | État de lumière/temps utilisé par la scène |
| `shaders/` | Calcul réel des matériaux, sky et échantillonnages |

### Réflexions

La branche possède des cubemaps dynamiques double-bufferisées, un framebuffer
de capture et un état de sonde avec origine et bornes de box projection.
Une réflexion intérieure dépend de la position de la sonde et de la
correction de parallaxe ; changer seulement la taille de texture ne corrige
pas un reflet « étiré » dans le garage.

La capture, l’écriture des faces, la publication d’une cubemap prête et son
échantillonnage ont des durées de vie distinctes. Vérifier les six orientations,
le repère, les transitions de monde, les uniforms et la validité de la sonde.
Ne pas lire une texture simultanément comme destination de sa propre capture.

Une option « dynamic reflections » cochée ne prouve pas que le matériau voiture
lit cette cubemap. Suivre le binding jusqu’au shader. Vérifier les fallbacks
vers les anciennes envmaps et les flags `vehicle_material`.

### Ombres

Les ombres de carte et de voiture ont des passes et ressources à examiner
séparément. Enhanced vise les ombres dynamiques, pas la simple superposition
de l’ancienne ombre projetée SLRR et d’une nouvelle shadow map.

La résolution seule n’explique pas les pixels : couverture en mètres,
projection, profondeur, biais, filtrage, alpha des feuilles et choix des
casters comptent aussi. Un bâtiment absent des casters restera sans ombre
même avec une shadow map plus grande.

Ne pas collecter les casters uniquement avec le frustum de la caméra visible.
La liste doit respecter le volume de lumière utile. Lors d’un changement
d’heure, les matrices et caches dépendants du soleil doivent être invalidés.

### Ciel et lumières

Le ciel Enhanced doit être piloté par le temps de jeu et des paramètres
cohérents avec le soleil, les nuages et la réflexion. Une apparence nocturne
ne doit pas laisser un soleil de midi dans les matériaux ou l’ombre.
Les paramètres de nuages animés nécessitent un temps stable, distinct d’un
nombre d’images écoulées.

Les surfaces OSD ont leur propre contrat de blending. Ne pas leur appliquer
l’éclairage du monde ou une correction globale destinée à la carrosserie.
Pour les arbres et vitres, vérifier alpha test, blending, depth write, ordre
des passes et mipmaps avant de modifier les textures originales.

## 13. Interface, entrées et options

Les registres OSD, widgets script et rectangles de picking doivent rester
synchronisés. Distinguer état sélectionné, hover et indisponibilité. Une
texture commune à deux boutons n’en fait pas le même widget.

Les entrées passent par `host_input` et les natives Input/Controller :
mapping, listener de redéfinition, axes, hotkeys et focus. Ne pas contourner
le mapping avec une touche codée en dur dans la conduite. Le listener doit
permettre les commandes primaires/secondaires et les autres menus de controls.

Les instruments voiture peuvent provenir du layout stock ou du CFG. Les
slots `HudSlot` couvrent notamment compteurs, aiguilles, kilométrage, dégâts,
nitro et turbo. Préserver taille, aspect et coordonnées attendus ; une image
de référence décalée à grande résolution peut refléter un comportement stock
volontairement conservé, pas un élément à recentrer automatiquement.

`host_game_options` centralise les options de conduite, vidéo et gameplay.
Le stockage stock est `save/game/options`. L’application live d’une résolution
doit synchroniser Config, mode host, dimensions de framebuffer, viewport et
ressources dépendantes de la taille. Relire une ancienne valeur à l’ouverture
d’un menu ne doit pas ramener le jeu en 800×600.

`EnhancedGraphics` contient les options supplémentaires partagées avec les
menus/debug. Toute nouvelle native d’option doit avoir un descripteur stable,
un comportement borné, une stratégie de sauvegarde définie et une documentation
dans le patch livré. Ne pas annoncer une option persistante avant de tester
un redémarrage.

## 14. Audio et vidéo

`host_sfx` gère les effets ; `host_music` les playlists ; `video_fmv*` la
lecture vidéo. Les boucles moteur utilisent les SfxTable des scripts.
Ne pas substituer un son moteur fixe à une courbe de régime/charge.

Les voix simultanées sont volontairement bornées et les répétitions de menu
traitées séparément. Installer un kit peut provoquer beaucoup d’événements
d’installation : éviter leur addition incontrôlée, sans supprimer tous les
retours sonores. Les sons de hover rapides doivent conserver un petit
chevauchement borné, pas une boucle alimentée par un doublon d’événement.

Pause, changement de section et destruction d’une voiture doivent mettre à
jour les boucles associées. Arrêter le rendu d’une voiture ne suffit pas à
arrêter son moteur audio.

Le processus est x64 : une DLL codec 32 bits ne peut pas être chargée dans
ce processus. Examiner les backends vidéo présents et leurs dépendances
réelles avant de promettre un codec ou une résolution. Garder le volume FMV,
musique et effets correctement séparé selon les options.

## 15. Sauvegarde et transitions

La section active a un cycle exit/enter. `SectionMachine` peut aussi adopter
un état que Java a déjà transitionné : ne pas appeler exit/enter une seconde
fois dans ce cas. L’état de présentation retient une frame complète pendant
certaines transitions coopératives pour éviter les flashes de scène détruite.

La voiture de prévisualisation, celle sélectionnée en mode libre et celle
chargée depuis une carrière ne sont pas interchangeables. Un bypass de
sauvegarde/chargement ne doit pas promouvoir une instance de preview en
voiture de carrière. Diagnostiquer le flux stock, les références et les
fichiers réellement chargés avant d’ajouter un cas spécial CarSelector.

Réinitialiser une section signifie examiner ensemble : monde parent, pose,
vitesses, accumulateur physique, contacts, assemblage, caméra, lumières,
réflexions, ombres et état de loading. Ne pas garder la pose tombée d’une
ancienne voiture comme pose initiale de la suivante.

L’aller-retour catalogue/garage ne recrée pas forcément le même ensemble
d’objets qu’un aller-retour dealer. Un état réparé par le dealer mais pas
par le catalogue indique souvent une invalidation ou une réinitialisation
manquante ; ce n’est pas une raison pour recharger le monde à chaque frame.

## 16. Méthode de travail et validation

### Avant de modifier

1. Lire `git status`, la branche et le diff existant dans `host/`.
2. Identifier le script appelant, la signature et le propriétaire de l’état.
3. Reproduire un scénario réduit avec une voiture/section connue.
4. Tracer les entrées, transitions et destructions utiles, sans spam par frame.
5. Corriger la couche responsable puis vérifier ses clients voisins.

Ne pas annuler les modifications d’un autre agent ou de l’utilisateur pour
revenir à une base supposée propre. Pour reprendre un correctif d’une autre
branche, appliquer ses hunks ou faire une intégration Git ; restaurer un
fichier entier depuis cette branche peut effacer les évolutions Enhanced.

### Compiler

Depuis `host/`, avec Visual Studio 2022, CMake et les dépendances configurées :

```powershell
cmake -S . -B out/build-bgfx-x64 -A x64
cmake --build out/build-bgfx-x64 --config Release --parallel 8
.\out\build-bgfx-x64\Release\slrr_host.exe --game --root ..\game --window
```

Un cache CMake existant doit conserver son générateur. Vérifier le README
pour les prérequis et dépendances. Le shader compilé doit correspondre à la
source modifiée ; ne pas valider un vieux binaire simplement parce qu’il démarre.

Si le jeu utilise l’exécutable, construire un candidat séparé ou attendre sa
fermeture pour le remplacer. Ne pas arrêter la partie de l’utilisateur sans
instruction. Éviter de renommer globalement les cibles de dépendances lors
d’un build candidat.

### Vérifications ciblées

```powershell
cmake -S . -B out/build-bgfx-x64 -DBUILD_TESTING=ON
cmake --build out/build-bgfx-x64 --config Release --target slrr_tests --parallel 8
ctest --test-dir out/build-bgfx-x64 -C Release --output-on-failure
```

Les tests sont optionnels au build. Les suites actuelles ne valident pas
l’ensemble du jeu : elles ciblent les plages arraycopy, les PTX malformés et
les sessions/courbes dyno avec le GC. Les diagnostics `--probe-dyno` et
`--probe-gc` complètent certains scénarios ; consulter `main.cpp` pour les
arguments acceptés.

Pour un correctif UI, tester interaction et focus. Pour une transition,
répéter l’aller-retour et observer les compteurs d’instances/draws/mémoire.
Pour le rendu, comparer même endroit, heure, caméra et paramètres. Pour la
physique, comparer temps simulé, input et voiture identiques à plusieurs FPS.

Un diagnostic peut modifier les options ou une sauvegarde. Utiliser une copie
ou préserver/restaurer les fichiers concernés. Un test headless ne remplace
pas une validation visuelle et ne doit pas être présenté comme telle.

### Livrer

Indiquer la cause, les fichiers concernés, la validation réellement faite,
les limites et les fichiers patch nécessaires. Ne pas actualiser les chiffres
de couverture native à partir du seul nombre de fonctions C++ modifiées.
Documenter toute nouvelle native : classe, nom, descripteur, paramètres,
effet, durée de vie et persistance éventuelle.

## 17. Diagnostic par symptôme

| Symptôme | Pistes prioritaires |
| --- | --- |
| FPS qui chutent après plusieurs voitures | Instances restées parentées, registres forts, GPU non libéré, boucles audio |
| Loading visible alors que la conduite fonctionne | Overlay/modal et cycle de présentation, pas uniquement vitesse de chargement |
| Hover ou son répété | Double dispatch host/script, widget caché encore actif, focus |
| Slider ferme une fenêtre | Mauvais handler/commande, événement sans widget, wake confondu avec fermeture |
| Voiture rétablie après installation d’une pièce | Rebuild/invalidation géométrie, contacts ou pose absent au chargement |
| Roues trop larges ou voilées | Slots, index roue, frame hub, ordre des rotations, double transformation |
| Reflets perdus au retrait d’une pièce | Recréation matériaux, binding cubemap, invalidation d’instance |
| Reflets déformés en intérieur | Origine sonde, box projection, repères et faces cubemap |
| Bâtiments sans ombre | Collecte casters, flags, passe et volume lumière |
| Ombres en gros pixels | Densité de texels, couverture, filtre, biais, alpha casters |
| Textures blanches | RID propriétaire, dépendances, slot, decode/upload |
| Textures multipliées ou z-fighting | Mesh/LOD en double, multipasses, UV et mélange matériau |
| Arbres clairsemés ou pointillés | Alpha test/blending, mipmaps, depth, matériau SCX |
| Retour à 800×600 | Config désynchronisé, restauration de valeurs stock, commit du dialogue |
| Preview qui apparaît en carrière | Identité et cycle save/load, rétention de l’instance précédente |

Ces pistes orientent l’enquête ; elles ne remplacent pas une preuve dans le
code ou un scénario reproductible.

## 18. Limites et références

La conduite, les dégâts, la ville et la carrière complète restent en
développement. Des APIs sont absentes ou partielles. Les compteurs de natives
et les anciens commentaires peuvent dater d’un état antérieur au rendu
Enhanced ; le code actif et les tests récents font foi pour l’implémentation.

Documentation locale complémentaire :

- [README](../README.md) : présentation, build et fonctionnalités.
- [SCRIPT_RUNTIME](SCRIPT_RUNTIME.md) : scheduler, notifications et modales.
- [VM_GC](VM_GC.md) : racines, safe points et finalisation.
- [NATIVE_AUDIT](NATIVE_AUDIT.md) : couverture et limites des bindings.
- [HOST_NATIVE_UTILITIES](HOST_NATIVE_UTILITIES.md) : utilitaires et options.
- [SCRIPT_COMPILER](SCRIPT_COMPILER.md) : compilation TUFA multiclasse et natives Language.

Références de travail externes, à comparer à une révision identifiée :

- `nedobylskiy/SLRR_rev` : reconstruction, formats, physique et télémétrie.
- `EliasFD3S/slrr-reverse` : reconstruction des APIs et comportements natifs.
- Scripts et classes du jeu installé : appelants concrets du contrat.
- Exécutable original et outils RE lorsque le code ne suffit pas.

Lorsqu’un comportement est seulement supposé, l’écrire comme hypothèse.
Lorsqu’une valeur ou un algorithme est porté, noter sa provenance et la
révision. Lorsqu’un contrat change, mettre à jour ce guide et la documentation
spécialisée correspondante dans la même livraison.
