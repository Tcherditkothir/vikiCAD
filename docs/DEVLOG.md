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

## 2026-07-07 — Clôture M5 : daily driver 2D = v1.0-2D ✅

- **Blocs** : BlockDef dans le Document (création directe, swap model-space journalisé), InsertEntity (pos/rotation/échelle uniforme/attributs JSON), AttDefEntity, `entityBounds()` insert-aware branché partout (extents/culling/hittest/snap). BLOCK convertit la sélection en bloc + insert ; INSERT demande les valeurs d'attributs ; EXPLODE matérialise (attributs → textes). Persistés en .vkd (owner_kind=1) — **bug attrapé au test de sortie : collision d'ids SQLite (rowid auto des entités de bloc vs ids explicites) → ordre d'écriture inversé**.
- **Réseaux associatifs** : ArrayEntity (clones prototypes + params rect/polaire + suppressions), régénération à chaque rendu, ARRAYRECT/ARRAYPOLAR (**leçon : paramètres AVANT la sélection**, sinon le glouton EntitySet avale les nombres), ARRAYEDIT clé/valeur journalisé (undo des paramètres testé), EXPLODE matérialise. DXF : export aplati (l'associativité vit dans le format natif — XDATA VIKI_ARRAY renvoyé à plus tard, consigné).
- **Sticky notes** 📌 : entité non géométrique (markdown, auteur, création/modification ISO, ancre point OU épinglée à une entité qu'elle suit), calque VIKI_NOTES auto non imprimable, NOTE/NOTEPIN/NOTEEDIT, `query --notes`. **DXF : POINT + XDATA VIKI_STICKYNOTE** (APPID enregistré, header JSON + texte chunké ≤240 o) — round-trip complet testé avec auteur/timestamps. **Patch vendored 0002** : `writeEntity` n'écrivait jamais les XDATA des entités (writeExtData jamais branché, signature incompatible).
- **Layouts + PDF** : Layout/Viewport persistés, LAYOUT name papier échelle|FIT (viewport unique marges 10 mm), PLOT + `export .pdf --layout` ; QPdfWriter 1200 dpi, échelle exacte (mm papier = mm modèle × échelle), épaisseur fixe 0,35 mm (v1), notes et calques non imprimables exclus. **Le CLI est passé en QGuiApplication offscreen** (les métriques de police du plot l'exigent).
- **IPC JSON-RPC 2.0** sur QLocalSocket 'vikicad' : ping/exec/query/open/save/screenshot ; `vikicad-cli connect <method>` réutilise la même surface — testé en live contre la GUI offscreen. QueryJson partagé CLI/IPC (une seule surface agent).
- **Raccourcis configurables** : ~/.config/vikicad/shortcuts.json (touche → ligne de commande), fichier d'exemple auto-créé.
- **71/71 tests verts.** Sortie M5 : dessin complet (blocs, réseau, cote, note) → .vkd → PDF 10 Ko → DXF → réimport avec note et blocs intacts → IPC live OK.

**v1.0-2D atteinte. Critère nanoCAD-désinstallé : à valider par Lex à l'usage (M6).**

Tag : `m5`.

## 2026-07-07 — M6 (partie automatisable) : perf + sanitizers ✅

- **Fichier torture 10k entités** (4k lignes, 3k cercles, 2k rects, 1k cotes) via script .vks — leçon au passage : mon générateur avait oublié la sémantique .scr (ligne vide pour terminer LINE).
- **Mesures (build DEBUG, CLI)** : création 10k commandes + save = 0,19 s ; load+bounds = 0,05 s ; load+JSON complet (1,7 Mo) = 0,10 s ; load+édit+save = 0,09 s ; export DXF = 0,14 s ; export PDF = 0,14 s ; réimport DXF = 0,13 s. **Porte de décision M6 : passée très largement — le scan linéaire et QPainter restent ; pas de R-tree nécessaire.**
- **ASan/UBSan** : a attrapé un **vrai use-after-free** dans TRIM/BREAK (copyStyle lisait l'entité source APRÈS removeEntity l'ait détruite — marchait « par chance » en pratique). Corrigé en capturant calque+couleur avant la suppression. 71/71 verts sous sanitizers.
- La partie « deux semaines d'usage réel quotidien » de M6 appartient à Lex — en cours en parallèle du développement 3D.

## 2026-07-07 — Clôture M7 : cœur 3D ✅

- **SolidEntity** : wrapper TopoDS_Shape, sérialisation BinTools en base64 dans le JSON (un seul sérialiseur pour .vkd ET l'undo — l'undo/redo des solides marche gratuitement, testé). Rendu 2D = empreinte XY + étiquette `[3D LxlxH]` ; la vraie visu est la vue OCCT. Transformations 2D relevées en 3D (rotation Z + translation + échelle uniforme, miroir → échelle négative).
- **Profils → fils OCCT** : cercles, polylignes fermées (segments + bulges → GC_MakeArcOfCircle), ellipses (aplaties), et **chaînage automatique** de lignes/arcs libres en boucles fermées. WORKPLANE XY|OFFSET z (plans sur face : différés à la GUI 3D, consigné).
- **EXTRUDE/REVOLVE/UNION/SUBTRACT/INTERSECT** : BRepPrimAPI_MakePrism/MakeRevol + BRepAlgoAPI ; profils consommés, résultat = solide journalisé. Volumes vérifiés exacts en tests (boîte, cylindre, triangle chaîné, Pappus pour la révolution, soustraction de perçage).
- **STEP** : STEPControl_Reader/Writer (TKDESTEP), un SolidEntity par SOLID à l'import, **sidecar `.vikinotes.json` toujours écrit/relu** (Plan B du plan de risques — les notes survivent au STEP). **OCCT écrivait ses statistiques sur stdout et corrompait le JSON du CLI** → messenger réduit au silence.
- **Vue 3D OCCT** : OcctViewWidget (AIS ombré, trièdre, rotation/pan/zoom souris) dans un QStackedWidget avec bouton « 3D » dans la barre d'état ; dégradation propre sans OpenGL/X (offscreen).
- **79/79 tests verts.** Sortie M7 : flasque construite en script (plaque + moyeu via UNION, alésage + perçage via SUBTRACT) → STEP 916 entités → réimport avec note sidecar intacte.

Tag : `m7`.

## 2026-07-07 — Clôture M8 : finition 3D ✅ — FIN DU PLAN INITIAL

- **FILLET3D / CHAMFER3D** : BRepFilletAPI sur toutes les arêtes du solide (v1 ; le picking d'arêtes individuelles dans la vue AIS est la prochaine évolution GUI). Rayon d'abord dans la grammaire (leçon du glouton EntitySet, 3e occurrence — pattern désormais systématique : paramètres avant sélection). Échecs OCCT (rayon infaisable) attrapés proprement. Undo BREP exact testé.
- **Plan A réussi** 🎉 : `VIKICAD_STEP_UDA=1` injecte les notes en **attributs utilisateur AP242 légaux** (PROPERTY_DEFINITION → REPRESENTATION → DESCRIPTIVE_REPRESENTATION_ITEM 'VIKI_STICKYNOTE' avec payload JSON) via `writer.WS()->Model()->AddWithRefs` après Transfer. Deux accrocs résolus : contexte de représentation null rejeté par le check du writer → réutilisation d'un contexte existant du modèle. Le sidecar `.vikinotes.json` reste TOUJOURS écrit (filet de sécurité, décision du plan de risques maintenue).
- Notes en 3D : les ancres 2D (plan XY) suffisent en v1 ; assemblages = stretch non bloquant, reporté.
- **80/80 tests verts.**

**Le plan initial M0→M8 est intégralement livré, en une session, le jour de sa conception.** Reste ouvert : M6-usage (les deux semaines de pratique réelle de Lex), et le backlog v2 consigné au fil du DEVLOG (pick-point hatch, MLEADER, plans de travail sur face, picking d'arêtes 3D, XDATA VIKI_ARRAY, éditeur MText in-canvas, dialogue DimStyle, R-tree si jamais nécessaire).

Tag : `m8`.
