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

## 2026-07-07 — Clôture M3 : outils de modification ✅

- **Intersections analytiques** (`geom/Intersect`) : seg×seg, ligne×cercle, seg×arc, cercle×cercle, arc×arc ; décomposition uniforme de toute entité en segments/arcs (`edit/EditOps`), ellipses/splines aplaties finement comme frontières.
- **TRIM/EXTEND** : cibles Line/Circle/Arc (polylignes : EXPLODE d'abord — restriction v1 assumée) ; frontières = tout type ; Enter = toutes les entités comme frontières ; boucle de picks.
- **OFFSET** : ligne/xline/cercle/arc/polyligne droite (joints à onglet par intersection, ouverte et fermée) ; côté par point.
- **FILLET R / CHAMFER D** : coin entre deux lignes, points de pick choisissant les moitiés conservées, R=0 = coin net ; l'arc de raccord est toujours le petit balayage.
- **BREAK** (intervalle ou scission), **JOIN** (lignes colinéaires → ligne ; chaînes lignes+arcs → polyligne à bulges, fermeture détectée), **EXPLODE** (polylignes → lignes+arcs), **STRETCH** (fenêtre de capture ; virtual `Entity::stretch`, sommets dans la fenêtre seulement), **MATCHPROP** (calque+couleur).
- **Conception clé** : tous les picks d'entité passent par un POINT + hittest avec `ctx.pickTolerance` (fixée par le zoom en GUI, dérivée des extents en headless) → les commandes d'édition marchent à l'identique en GUI, script et CLI. C'est ce qui a permis de tester TRIM/FILLET/… entièrement en headless.
- **Grips** : gripPoints/moveGrip par entité (ligne : extrémités+milieu-déplacement ; cercle : centre+rayon ; arc : extrémités/rayon/centre ; polyligne : sommets), drag avec fantôme + snap actif, édition journalisée.
- **Export DXF** (writer DRW_Interface) : toutes les entités M2, calques (ACI le plus proche + couleur vraie 24 bits, off = couleur négative, verrou = flag 4), $INSUNITS, versions R12→2018 (défaut 2013). Verbe CLI `export --dxf-version`.
- **Patch vendored 0001** : `dxfRW::writeSpline` n'écrivait jamais les fit points (codes 11/21/31) → les splines par points de fit revenaient vides du round-trip. Patché + documenté dans `patches/`.
- **Découverte R12** : libdxfrw jette silencieusement les LWPOLYLINE en R12 → fallback POLYLINE legacy dans l'exporteur ; l'ellipse y devient une polyligne 128 sommets (comportement libdxfrw, acceptable), la spline y est perdue (pas de record avant R13) — consigné dans le test.
- **55/55 tests verts** ; test de sortie : contour 100×60 + congé R12 + chanfrein 8×8 + cercle + TRIM + OFFSET, export DXF 2013, réimport 10/10.

Tag : `m3`.

## 2026-07-07 — Clôture M4 : annotation ✅

- **Pipeline de rendu étendu** : `TextPrimitive` (position/hauteur/rotation/alignement) + strokes remplis (`filled`) pour flèches et hachures pleines ; `RenderContext.doc` donne aux cotes l'accès styles+unités ; flag `forHitTest` pour la boîte de pick invisible des textes.
- **TextEntity** : multiligne (\n), hauteur/rotation/alignement, métriques approchées (0.62×hauteur/caractère) — **aucune dépendance police dans le core**, la GUI rend en vraie police avec LOD (barre sous 2 px). Éditeur in-canvas → différé, la barre de commande suffit en v1.
- **DimensionEntity** : Linear (axe auto H/V selon le placement)/Aligned/Angular (3 points, côté choisi par la position)/Radius/Diameter — **tout est régénéré** depuis les points de définition + DimStyle : le toggle mm/pouce reformate instantanément les cotes existantes (testé), DIMSTYLE reflow tout le dessin.
- **DimStyle** : 8 variables (TH/AS/EO/EB/GAP/DEC/SUF), table dans Document, persistée dans `.vkd` (table dim_styles), commande DIMSTYLE clé/valeur (dialogue GUI reporté — la commande est plus agent-friendly).
- **LeaderEntity** (flèche pleine + texte fin de chaîne) ; **HatchEntity** : anneaux aplatis stockés + SOLID/ANSI31/ANSI37, clipper pair-impair maison par rotation en espace pattern, garde-fou 5000 lignes. Trous non soustraits en SOLID (v1, consigné).
- **InputKind::Text avale le reste de la ligne** ; le mode strict envoie jusqu'à 3 Finish implicites (LINE répétant, texte optionnel de LEADER, défaut Y/N de MIRROR).
- **DXF** : TEXT/MTEXT (\P↔\n, une TEXT par ligne en R12), DIMENSION 5 types écrits **sans bloc *D** (les CAO régénèrent — décision du plan), LEADER classique + MTEXT séparé (le texte revient comme texte indépendant à l'import — dégradation assumée), HATCH en **boucles d'arêtes LINE** car le writer vendored n'implémente pas les boucles polyligne (« writeme » upstream — contourné sans patch).
- **63/63 tests verts** ; sortie M4 : pièce annotée complète (4 types de cotes + texte + hachure + leader) → export DXF 2013 → réimport 10/10, hachure et pattern intacts.

Tag : `m4`.
