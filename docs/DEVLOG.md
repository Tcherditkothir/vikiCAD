# VikiCAD — DEVLOG

Journal de développement. Une entrée par session de travail significative ; une entrée obligatoire à chaque clôture de jalon.

---

## 2026-07-07 — Entrée 0 : lancement du projet, squelette M0

**Contexte.** Migration Windows→Kubuntu 26.04 ; aucune CAO Linux satisfaisante pour du drafting 2D façon nanoCAD. Décision : construire VikiCAD. Plan complet validé (voir `~/.claude/plans/je-migtre-vers-linux-calm-nest.md`), jalons M0→M8, 2D daily-driver avant le 3D.

**Décisions d'architecture figées aujourd'hui :**
- Format natif SQLite `.vkd` (WAL) ; DXF/STEP = import/export uniquement.
- Renderer 2D dédié QPainter (pixmap statique + overlay dynamique) ; OCCT AIS réservé au mode 3D.
- Géométrie 2D analytique maison ; OCCT `Geom2d` en escalade ponctuelle (splines, ellipses) ; jamais de TopoDS stocké dans une entité 2D.
- Tout stocké en mm ; le toggle mm/pouce est purement présentation.
- `CommandProcessor` unique dans vikicore ; GUI/CLI/scripts/IPC = quatre adaptateurs du même moteur (abstraction `InputProvider`).
- Undo = journal transactionnel de deltas JSON enregistré par le Document (jamais d'undo par commande).
- Catch2 v3 ; le CLI headless sert de harnais d'intégration dès M1.

**Fait aujourd'hui :**
- Dépôt initialisé, arborescence complète (core/gui/cli/tests/third_party/docs).
- Superbuild CMake, 4 cibles : `vikicore` (lib statique sans QtWidgets), `vikicad` (fenêtre + barre de commande + canvas placeholder), `vikicad-cli` (`--version` en JSON + smoke test OCCT), `vikicad-tests` (Catch2 : Vec2d, version, lien OCCT).
- `libdxfrw` rendu optionnel à la configuration (guard `if(EXISTS)`) car le vendoring attend le réseau.
- Versions cibles constatées sur la machine : OCCT 7.9.2, Qt 6.10.2, CMake 4.2.3 (apt Kubuntu 26.04).

**Bloqueurs à la clôture de session :**
1. Machine hors-ligne (aucune route réseau) → vendoring libdxfrw impossible.
2. Toolchain absente (`build-essential`, `cmake`, `ninja`, Qt/OCCT dev) → installation apt requiert le mot de passe sudo ET le réseau.
→ Première compilation dès que ces deux points sont levés ; erreurs de build attendues à corriger à ce moment (noms de toolkits OCCT, config Catch2).

## 2026-07-07 — Clôture M0 ✅

Toolchain installée par Lex. Première compilation **propre du premier coup** (24/24 étapes, zéro warning avec -Wall -Wextra -Wpedantic), 4/4 tests verts, `vikicad-cli --version` sort le JSON attendu (OCCT 7.9.2 confirmé, smoke test BRepPrimAPI OK), la GUI démarre sans crash (vérifié en offscreen). Les noms de toolkits OCCT anticipés (TKernel/TKMath/TKBRep/TKPrim…) étaient corrects sur Ubuntu.

**Reste en suspens (reporté, non bloquant pour M1) :** vendoring libdxfrw — le réseau est retombé (probablement le WiFi AX210). Le DXF n'est requis qu'à M2 (import) ; à vendorer dès que le réseau revient.

Tag : `m0`.

## 2026-07-07 — Vendoring libdxfrw ✅

Réseau rétabli par Lex en Ethernet… qui s'avère **IPv6-only** (pas de bail DHCP IPv4) → GitHub injoignable. Contournement : le fork libdxfrw de LibreCAD extrait du tarball source Ubuntu `librecad_2.2.0.2.orig.tar.gz` (archive.ubuntu.com est derrière Cloudflare, IPv6 OK) — même code, pinné à la release 2.2.0.2. Provenance documentée dans `third_party/libdxfrw/README.vendoring.md`.

Cible statique `dxfrw` intégrée au superbuild (warnings vendored non durcis, include SYSTEM), `VIKICAD_HAS_DXF=ON`, build complet + 4/4 tests toujours verts. Aucun patch appliqué pour l'instant.

## 2026-07-07 — Clôture M1 : boucle de dessin minimale ✅

L'architecture porteuse du projet est en place, conforme au plan :
- **Document** : map d'entités + ordre de dessin, table de calques, **journal transactionnel d'undo** (deltas JSON Added/Removed/Modified enregistrés aux points de mutation — les commandes n'écrivent aucune logique d'undo, filet de sécurité rollback/commit dans le processeur).
- **Entités** Line/Circle/Arc : clone/bounds/transform/buildPrimitives/toJson ; le sérialiseur JSON unique sert au format natif, aux requêtes CLI et aux deltas d'undo, comme prévu.
- **CommandProcessor** unique + `InputValue`/`InputRequest`/`Step` ; commandes = machines à états (LINE répétant, CIRCLE centre+rayon-ou-point, ARC 3 points, ERASE pickfirst+ids, UNDO/REDO, ZOOM E). Grammaire : `x,y`, `@dx,dy`, `@d<angle`. Mode strict headless (Finish implicite) vs mode GUI suspendu.
- **NativeStore SQLite** (.vkd, WAL, application_id VIKD, user_version 1) : save = réécriture en une transaction ; round-trip byte-identical testé, ids stables préservés.
- **ScriptRunner .vks** : sémantique .scr (valeurs sur lignes suivantes, ligne vide = Enter).
- **CLI** : `new/open --exec/--run/--save/--save-as`, `query --entities/--layers/--bounds`, tout en JSON une-ligne.
- **GUI** : Camera2d (monde mm y-haut ↔ écran px), canvas pixmap statique + overlay (preview pointillé des commandes, surbrillance sélection, rubber band fenêtre L→R / capture R→L, réticule), pan MMB, zoom molette ancré au curseur, ESC/Enter, menus fichier.

**22/22 tests verts** (géométrie, document/undo, commandes headless, scripts, store). **Test de sortie M1 réussi** : script .vks → 6 entités → save → reopen → exec (ajout/erase/undo) → save → query JSON complet avec ids et bounds.

Erreurs de compilation rencontrées (2, triviales) : `std::clamp` sans `<algorithm>` ; fonction nommée `emit` (macro Qt) → `emitJson`.

Tag : `m1`.

## 2026-07-07 — Clôture M2 : drafting de précision ✅

- **Nouvelles entités** : Polyline (segments à bulge conformes DXF — bulge>0 = CCW, vérifié contre les formules de référence), Ellipse (+arcs elliptiques), Spline (NURBS de Boor rationnel, fallback honnête en corde par points de fit), Point (glyphe taille-écran), XLine (infinie, clippée au viewBox, exclue des extents). Commandes RECT/PLINE(+C)/ELLIPSE/POINT/XLINE/SPLINE.
- **Unités** : `parseLengthToken` avec suffixes `"` /`in`/`mm` ; les nombres nus suivent l'unité d'affichage ; nouveau `InputKind::Number` brut pour angles/facteurs (pas de conversion). Bouton mm/in dans la barre d'état, readout de coordonnées converti.
- **SnapEngine** : candidats typés émis par les entités (endpoint/midpoint/center/quadrant), intersections par paires sur flatten local, perpendiculaire depuis le point de base ; priorité endpoint > intersection > midpoint > center > quadrant > perp ; glyphes overlay distincts par type. **Décision** : préfiltre bbox linéaire au lieu du R-tree (µs à 10k entités) — même interface, R-tree si le profil M6 le réclame.
- **Modes** : ORTHO (projection axe), POLAR (crans 15°), GRID (snap + affichage lignes mineure/majeure/axes), boutons bascule dans la barre d'état. L'osnap prime sur ortho/grille.
- **MOVE/COPY/ROTATE/MIRROR/SCALE** : classe de base ModifyCommand (étape de sélection uniforme pickfirst/ids/picks), transformation journalisée en une transaction, aperçu fantôme au curseur. Miroir : matrice de réflexion dérivée proprement, bulges inversés, arcs/ellipses re-paramétrés.
- **Calques** : table dans Document (ensureLayer/setLayerProps/removeLayer/currentLayer — opérations directes non journalisées, choix v1 documenté), persistées dans le .vkd, LayerPanel (courant/nom/couleur/visible/verrou), verrou = visible mais non sélectionnable.
- **PropertiesPanel** : calque et couleur de la sélection appliqués en transaction journalisée.
- **Import DXF** : DxfImporter sur DRW_Interface, table ACI de libdxfrw réutilisée (DRW::dxfColors), calques (gelé/off/verrouillé), $INSUNITS, types non supportés comptés et rapportés. **Test réel : screw2012ascii.DXF (fourni par libdxfrw) → 27 entités, 3 calques, 0 sautée.** Verbe CLI `import` + File→Import DXF.
- **43/43 tests verts.** Erreur de conception attrapée par les tests : mon intuition du sens du bulge était inversée — la spec DXF (bulge+ = CCW = à droite du sens de parcours) a été re-dérivée et le test corrigé, pas le code.

Tag : `m2`.
