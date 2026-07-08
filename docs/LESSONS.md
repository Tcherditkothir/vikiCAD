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
- **Leçon (grammaire de commandes)** : le glouton EntitySet (qui avale tous les tokens numériques) a mordu TROIS fois (ARRAYRECT, test store, FILLET3D). Règle désormais gravée : **dans toute commande headless, les paramètres numériques viennent AVANT la sélection d'entités.**
- **Leçon (sanitizers)** : ASan a attrapé un use-after-free réel (TRIM/BREAK) qui passait tous les tests fonctionnels. La passe sanitizers avant chaque tag n'est pas du luxe.
- **Leçon (bulge DXF)** : bulge positif = arc CCW = à DROITE du sens de parcours pour une corde +x. Mon intuition disait l'inverse ; la dérivation (3 méthodes concordantes : spec, Lee Mac, QCAD) a tranché contre le test, pas contre le code.
- **Leçon (libdxfrw)** : trois lacunes réelles trouvées et traitées — fit points de spline jamais écrits (patch 0001), XDATA d'entité jamais écrits (patch 0002), boucles polyligne de hatch « writeme » (contourné par boucles d'arêtes). Le choix « vendored patchable » du plan était le bon.
- **Leçon (OCCT)** : STEPControl écrit ses statistiques sur stdout — mortel pour un CLI JSON ; museler `Message::DefaultMessenger` d'entrée. Et le Plan A AP242 (attributs utilisateur) est passé du premier coup une fois le contexte de représentation non-null — plus facile que craint, le sidecar reste quand même.
- **Leçon (méta)** : le plan initial complet (M0→M8) a tenu en une session parce que chaque jalon finissait testé-vert et taggé avant d'ouvrir le suivant, et parce que le CLI-harnais de test existait dès M1 — les deux meilleures décisions du plan.
- **Leçon (fausse piste)** : « le logiciel se ferme tout seul » n'était PAS un bug de VikiCAD — c'étaient les instances GUI lancées par l'agent depuis ses commandes sandboxées, fauchées à la fermeture du sandbox. Diagnostic par élimination : lancement via `systemd-run --user` → stable indéfiniment. Règle : l'agent lance la GUI en unité systemd (`vikicad-gui`) ; les lancements utilisateur (menu/icône) n'ont jamais été affectés.
- **Leçon (conventions d'angles DXF — le « massacre » du DWG)** : libdxfrw N'EST PAS cohérent entre entités : INSERT code 50 → converti en RADIANS au parse ; TEXT code 50 → laissé en DEGRÉS ; MTEXT code 50 → brut (la spec DXF dit radians). Mon importeur traitait tout en degrés → toutes les rotations de blocs divisées par ~57 → dessin « croche ». S'ajoute : xscale≠yscale et échelles négatives (blocs miroirés) écrasées par mon échelle uniforme. Règle : à CHAQUE champ libdxfrw, vérifier le commentaire du header ET le parseCode. Round-trip testé rotation+miroir+échelle.

## 2026-07-08

- **Leçon (MTEXT code 71)** : dans libdxfrw, l'attachment point du MTEXT atterrit dans `textgen` (hérité de DRW_Text via parseCode 71), PAS dans `alignV` malgré le commentaire du constructeur. Les deux chemins (DXF parseCode et DWG parseDwg) sont cohérents là-dessus. Toujours vérifier le parseCode, pas les commentaires.
- **Leçon (cotes DXF)** : des « textes géants » sur un dessin importé ne sont pas forcément un bug de texte — c'étaient des DIMENSION régénérées avec notre DimStyle par défaut (3,5 mm) dans une zone dessinée à ~0,01 unité. Trois mécanismes distincts à couvrir : la table DIMSTYLE du fichier (tailles absolues ×DIMSCALE), l'override de texte code 1 (avec `<>` = valeur mesurée), et l'échelle d'insertion des blocs (styleScale suivi par transform). Les trois manquaient.
- **Leçon (snap et perfs)** : pour snapper dans les blocs, transformer les POINTS de snap des sous-entités (xf.apply) au lieu de cloner+transformer les entités — même exactitude pour tous les types de points, et aucun coût mémoire par requête souris.
- **Piège (heredoc bash)** : un fixture DXF contient une ligne littérale `EOF` — un heredoc `<< 'EOF'` se fait couper au milieu. Utiliser un délimiteur improbable (`ENDOFTEST`).
