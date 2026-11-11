# Guide de travail des agents

Avant une modification moteur, lire [ENGINE_CONTRACT.md](ENGINE_CONTRACT.md).
Ce guide décrit le contrat SLRR, les responsabilités des couches et les pièges
de compatibilité. Lire ensuite les sections spécialisées correspondant au sujet :

- VM, threads, événements et dialogues : [SCRIPT_RUNTIME.md](SCRIPT_RUNTIME.md).
- Durée de vie des objets : [VM_GC.md](VM_GC.md).
- Bindings, no-ops et limites : [NATIVE_AUDIT.md](NATIVE_AUDIT.md).
- Build et lancement : [README.md](README.md).

Vérifier le code actif : certains audits et commentaires décrivent des états
plus anciens. Préserver les modifications déjà présentes dans le working tree.
Corriger les primitives générales plutôt qu’ajouter des exceptions de VM pour
une voiture ou un écran. Une modification Java doit respecter le périmètre
autorisé par l’utilisateur et être incluse dans le patch livré si nécessaire.

Mettre à jour la documentation concernée lorsqu’un contrat ou une responsabilité
change. Rapporter distinctement compilation, tests automatisés et validation
en jeu ; aucun de ces résultats ne prouve à lui seul la compatibilité complète.
