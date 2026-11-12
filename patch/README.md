# OpenSLRR Enhanced

Copier le dossier `sl` de ce patch à la racine du jeu et accepter le remplacement de `OptionsDialog.class`. La source Java est fournie avec la classe compilée. Utiliser la Release Enhanced v3.

Le menu **Video Options** applique la résolution et les réglages Enhanced en jeu. La portée du décor extérieur est de 5 km. Les cartes d’ombres du décor et des véhicules utilisent chacune une résolution de 2048 × 2048. **F12 → Renderer** permet aussi de régler le nombre de faces de réflexion calculées par image.

## Natives Java

| Classe et signature | Comportement |
| --- | --- |
| `java.game.OptionsDialog.getRendererOption(I)I` | Lit la valeur active pour l’identifiant demandé. Un identifiant inconnu renvoie 0. |
| `java.game.OptionsDialog.setRendererOption(II)V` | Applique la valeur à la session. Les identifiants inconnus sont ignorés. |

| ID | Option | Valeurs |
| --- | --- | --- |
| 0 | Anti-aliasing | 0 désactivé, 1 MSAA 2×, 2 MSAA 4×, 3 MSAA 8×. Les autres valeurs sont bornées à cet intervalle. |
| 1 | Reflets dynamiques | 0 désactivé, valeur non nulle activée |
| 2 | Nuages volumétriques | 0 désactivé, valeur non nulle activée |
| 3 | Lumière solaire du décor | 0 désactivé, valeur non nulle activée |
| 4 | Lumières locales | 0 désactivé, valeur non nulle activée |

Le menu et ImGui partagent ces valeurs. Elles sont réinitialisées au prochain lancement. La résolution, l’activation globale des ombres et les flares utilisent la configuration existante du jeu.

## Piétons

Le curseur **Pedestrian density** est retiré. La foule reste au maximum d’origine du jeu (1,5). Le jour et la nuit gardent le coefficient de la carte.
