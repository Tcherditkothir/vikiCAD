# VikiCAD — Erreurs et leçons apprises

Log continu des erreurs commises, impasses, et leçons techniques. Ajouter au fil de l'eau, ne jamais réécrire l'historique.

---

## 2026-07-07

- **Leçon (recherche préalable) :** OCCT n'a AUCUNE voie supportée pour exporter des notes texte AP242 (`XCAFDoc_NotesTool` ne traduit pas vers STEP). Décidé d'avance : sidecar JSON d'abord, injection d'UDA AP242 time-boxée plus tard. Ne pas redécouvrir ça en M7.
- **Leçon (environnement) :** toujours vérifier réseau + sudo AVANT de planifier une session d'installation ; la machine peut être hors-ligne. Le squelette a pu être écrit intégralement hors-ligne grâce au guard `if(EXISTS)` sur libdxfrw — pattern à retenir pour tout vendoring.
- **À vérifier à la première compilation :** noms des toolkits OCCT 7.9 sur Ubuntu (TKSTEP → TKDESTEP depuis 7.8 pour l'échange de données — le lien actuel de vikicore n'utilise que les toolkits de modélisation, à étendre en M7). → Vérifié : les toolkits de modélisation portent les noms attendus.
- **Piège Qt :** ne jamais nommer une fonction `emit` — c'est une macro Qt (keywords). Renommée `emitJson` dans le CLI.
- **Leçon (conception script)** : la première version du ScriptRunner soumettait chaque ligne en mode strict, ce qui cassait les commandes multi-lignes façon .scr d'AutoCAD (`LINE` sur une ligne, points sur les suivantes). Corrigé avant même la compilation en relisant le comportement attendu. Les sémantiques d'AutoCAD sont la référence de moindre surprise pour ce projet.
- **Leçon (undo)** : le choix « journal enregistré par le Document » a tenu ses promesses dès M1 — ERASE/UNDO/REDO ont fonctionné sans écrire une ligne d'undo dans les commandes, et le filet « rollback si transaction ouverte à l'annulation » a un test dédié.
