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

## 2026-07-07 — M6-usage, première session réelle de Lex

**Premier vrai fichier ouvert** : `Ligne de temps.dxf` (clé USB) — 19 340 entités, 26 calques, import en ~0,1 s, 92 entités sautées (viewports papier, solid-fills). Rendu correct. Retours de Lex et actions :

1. **« On se croirait au début des années 2000 »** → thème sombre complet (Fusion + palette + stylesheet : docks, tables, boutons d'état, barre de commande monospace à prompt accentué). `gui/Theme.cpp`.
2. **STEP réel** : `BASE_BLACK.step` (clé USB) importé → 1 solide, bornes cohérentes. ✅
3. **« Ma bibliothèque est surtout en DWG »** — le DWG n'était PAS au plan (DXF seulement). Solution en deux étages :
   - `importDwg()` via le lecteur DWG de libdxfrw (libdwgr, R14→2013) — même pipeline Builder que le DXF, dispatch par extension partout (CLI + GUI).
   - **Découverte** : la Pyramide de Lex est en DWG **AC1032 (2018)**, au-delà de libdwgr. Fallback : conversion via `dwg2dxf` de **GNU LibreDWG** (compilé depuis ftp.gnu.org — pas packagé Ubuntu ; premier essai cassé par -Werror sous GCC 15, recompilé avec -Wno-error) détecté dans PATH/~/.local/bin, QProcess + import du DXF temporaire.

## 2026-07-08 — Le bug du plant de tomates 🍅 (patch vendored 0004)

`Domaine_Bichonnerie_BelleMaison.dwg` : 44 calques importés mais **0 entité modèle sur 12 522** — sans aucune erreur. Traque : instrumentation des callbacks (addBlock/endBlock équilibrés), puis du setError du lecteur (jamais appelé), puis dump binaire du point de mort. **Cause** : un MTEXT de description de plants de tomates contenait un retour à la ligne littéral ; dwg2dxf (LibreDWG) l'écrit BRUT dans le DXF. La ligne de débordement atterrit là où le lecteur attend un code de groupe, `atoi(" soil moisture.")` = code 0 = « fin d'entité », et TOUT le reste du fichier est lu décalé d'une ligne — silencieusement. Patch 0004 : une ligne de code doit être numérique ; les lignes non numériques sont sautées comme continuations de valeur. Résultat : 12 522/12 522 importées.

Leçon : dans un pipeline de conversion, le silence n'est pas un succès — 0 entité + ok:true doit devenir un warning visible (fait : le CLI le signale désormais ? à ajouter).
## 2026-07-08 — M6-usage : snap dans les blocs, MTEXT/TEXT alignés, cotes à l'échelle

Reprise post-compaction sur les deux chantiers ouverts (retours de Lex : « beaucoup de travail sur les snaps », « encore un peu d'erreurs sur les blocs de texte »).

1. **Snap dans les blocs** (le gros manque identifié) : `snapQuery` récurse désormais dans les définitions de blocs — les points de snap des sous-entités sont transformés par la matrice d'insertion (pas de clonage d'entités : les points se transforment exactement, µs par requête même sur les gros blocs). Blocs imbriqués jusqu'à profondeur 4. Le point d'insertion reste snappable. Tests : bloc tourné/échellé, bloc imbriqué.
2. **MTEXT/TEXT** : `TextEntity` gagne `vAlign` (Baseline/Top/Middle/Bottom) et `lineSpacing` par entité. Import : attachment point MTEXT (code 71), interligne (code 44, ×5/3), justification TEXT (codes 72/73 → ancre au point d'alignement code 11), et **décodage complet des codes inline** (`{\f...;}`, `\H...x;`, `\S` fractions → « a/b », `\U+XXXX`, `%%d/%%c/%%p`) — l'ancien cleanMtext ne traitait que `\P`. Export symétrique (TEXT justifié, MTEXT attachment+interligne ; Baseline → Top avec ancre relevée d'une hauteur de capitale pour un round-trip géométrique exact).
3. **Cotes** (découvert en validant sur Bichonnerie : chiffres géants sur les vignettes McMaster) :
   - la table **DIMSTYLE** est maintenant importée (dimtxt/dimasz/dimexo/dimexe/dimgap ×DIMSCALE, dimdec) et chaque cote garde sa référence de style ;
   - l'override de texte (code 1) est importé, `<>` substitué par la valeur mesurée au rendu, override blanc = texte supprimé ;
   - `DimensionEntity`/`LeaderEntity` gagnent `styleScale` multiplié par les transformations → les cotes dans des blocs échellés gardent leurs proportions.
4. **ZOOM W** (fenêtre, deux coins ou W explicite) — nécessaire pour valider par capture d'écran via IPC, et utile au quotidien.

Validation : Domaine_Bichonnerie 12 522 entités réimporté, vignettes de quincaillerie McMaster maintenant lisibles avec cotes proportionnées. **90/90 tests verts, passe ASan ciblée propre.** Commit `82d63a7`.

## 2026-07-08 (suite) — Troncature MTEXT réparée + menu snaps par type

- **Troncature de texte** (retour Lex : « il y a toujours des manquements » sur `Domaine_Bichonnerie`) : le MTEXT « Calcul de distances » finissait à « Immeuble protég ». Cause : `dwg2dxf` enveloppe les valeurs longues à **254 octets** en insérant un CR/LF **au milieu de la valeur** (ici au milieu de « protégé ») ; la ligne de débordement n'a pas de code de groupe. Le patch 0004 initial *sautait* ces lignes (corrigeait le décalage de parse « bug tomate ») mais les **jetait** → texte tronqué. Révision : lookahead d'une ligne (`nextRawLine`/`pushRawLine`) qui **re-colle** la continuation octet pour octet (le newline injecté est retiré), conditionné à la largeur d'enveloppe (segment ≥ 254 ET ligne suivante non numérique). Le DXF bien formé n'est jamais touché (une valeur y est toujours suivie d'un code numérique). Résultat : « Immeuble protégé = 1,0 / Maison d'habitation = 0,5 / Périmètre d'urbanisation = 1,5 » revient en entier. 12 522 entités inchangées, tous les fichiers réimportent à l'identique, 90/90 tests + 2 nouveaux tests lecteur, ASan propre.
- **Menu snaps par type** (retour Lex) : clic droit sur le bouton SNAP de la barre d'état → menu avec une case à cocher par type (Endpoint/Midpoint/Center/Quadrant/Intersection/Perpendicular) + « Select all / Clear all », câblé sur les champs de `SnapSettings` que le canvas lit à chaque déplacement du curseur. F3 garde le on/off global.

## 2026-07-08 (suite 2) — Répéter/clic-droit, ellipses, édition texte, layers, ENGRENAGES

Lot de retours Lex traités :

- **Répéter la dernière commande** : clic droit sur le canvas = répéter (à vide) ou Enter (en cours de commande) ; le canvas prend le focus au clic pour que Entrée/Espace répètent de façon fiable.
- **Objets mal orientés** (Bichonnerie) : deux bugs. (1) `EllipseEntity::transform` supposait une similitude uniforme et stockait un ratio NÉGATIF au miroir → ellipses mal orientées dans les inserts miroités/échellés + export DXF corrompu. Remplacé par la vraie transformation affine (demi-diamètres conjugués → axes principaux ; rotation, échelle non uniforme ET miroir ; ratio toujours positif ; params d'arc remappés). (2) 19 demi-ellipses avaient une extrusion (0,0,-1) → balayage inversé → mauvaise moitié dessinée. Réflexion des params à l'import quand extrusion Z<0.
- **Édition des textes** : double-clic sur un texte → dialogue (contenu multi-ligne, hauteur, alignements H/V) en transaction annulable ; commande `TEXTEDIT` (alias ED/DDEDIT) pour CLI/agents.
- **Panneau Layers** : colonnes triables (clic sur l'en-tête ; le mapping ligne→calque passe par l'id stocké donc le tri est sûr) ; docks flottants agrandissables (chrome de fenêtre min/max/close).
- **Outil d'engrenages** 🎉 (`core/geom/GearGeometry`, commande `GEAR`/`SPURGEAR`) : profil de denture à développante (module, dents, angle de pression, addendum/dedendum, alésage) généré en polyligne fermée, avec garde anti-auto-intersection (dents pointues à faible z). Génère AUSSI une **sticky note de conception** : toutes les cotes (Ø primitif/base/tête/pied, pas, épaisseur…), l'accouplement (même module+PA, entraxe, ratio) ET le raisonnement de conception (nombre de dents vs sous-taille/résistance, module, angle de pression 14,5/20/25°, largeur de denture, vitesse/puissance via Lewis/Hertz). Rendu validé (18 dents + alésage), 99 tests verts.

- **Aperçus live** (retour Lex : « pas d'aperçu avant que je conclue ») : ajout de `previewAt` là où il manquait sur les commandes les plus utilisées. OFFSET affiche la courbe décalée qui suit le côté du curseur (refactor : `editops::offsetGeometry` produit la géométrie sans muter le doc, partagée par la commande et l'aperçu). GEAR prévisualise toute la denture (+ alésage) au curseur une fois les paramètres saisis. ELLIPSE (axe majeur en rubber-band puis ellipse au ratio du curseur) et SPLINE (courbe à travers les points + curseur). Restent sans aperçu (suite) : FILLET/CHAMFER/TRIM/EXTEND (survol/sélection plus complexes).

- **File > Open ouvre DXF/DWG/STEP directement** (retour Lex : « pourquoi je ne peux pas ouvrir un dwg direct ? »). Avant, `Open` ne lisait que le `.vkd` (l'import était caché dans un menu séparé) → « not a VikiCAD file (no entities table) » sur un DWG. Unifié : `loadFile()` aiguille par extension (.vkd → NativeStore, .dxf/.dwg → import libdxfrw/dwg2dxf, .step/.stp → importStep), avec dialogue d'erreur en interactif. Le filtre du dialogue liste tous les types ; le verbe IPC `open` passe par le même chemin (0 entité importée → warning explicite). `connect open planifplusSV.dwg` → 41 669 entités.

- **Ouvrir un STEP → vue 3D directe** (retour Lex : « quand j'ouvre un STEP » on ne voit que des rectangles « [3D LxlxH] »). Un dessin fait uniquement de solides (STEP importé) n'a aucun sens en 2D — le canevas n'affiche que les empreintes. `adoptDocument` bascule maintenant automatiquement en vue 3D quand le document est solides-uniquement (`documentIsSolidsOnly`), avec un rappel des contrôles orbit/pan/zoom. Le bouton 3D est mémorisé (`m_3dButton`) et re-synchronisé. Ajout : verbe IPC `view3d` (on/off) et capture d'écran de la vue courante — la vue 3D via `V3d_View::Dump` (QWidget::grab ne capture pas la fenêtre GL native d'OCCT). Vérifié : ELECTRONIK_BLACK_N_WHITE.step (15 solides) s'ouvre directement en 3D ombré isométrique. Le rendu 3D lui-même est bon ; restait juste à ne pas rester coincé en 2D. Suite (3D interactif) : highlight au survol des arêtes/faces, sélection, assemblages.

## 2026-07-08 (suite 3) — 3D interactif : STEP→3D, sélection, assemblage

- **Ouvrir un STEP → 3D directe** : `documentIsSolidsOnly()` bascule en vue OCCT à l'ouverture (empreintes 2D inutiles pour des solides). Bouton 3D mémorisé/synchronisé, verbe IPC `view3d`, capture 3D via `V3d_View::Dump` (grab() ne capture pas la fenêtre GL native).
- **Sélection 3D interactive** : modes de sélection whole-solid/face/edge activés ; survol = highlight dynamique (AIS MoveTo) ; clic (vs drag=orbit) = sélection (SelectDetected), rapportée dans la barre de commande via le signal `picked()`.
- **Assemblage** (commande dédiée choisie par Lex) : champ `component` sur SolidEntity (persisté) ; **import STEP additif** (`File > Insert STEP as component` + verbe IPC `insertstep` + helper `insertStepFile`) qui ajoute les solides au document courant en les taguant du nom de fichier ; **panneau Assembly** (arbre composants→solides, clic = sélection/highlight) ; commandes **MOVE3D** (dx dy dz) et **ROTATE3D** (axe X/Y/Z + angle) via `SolidEntity::applyTrsf(gp_Trsf)` (transaction annulable). Validé : MOTOR_FIX + BASE_BLACK réunis en un seul modèle 3D.
- **Bug corrigé** : `insertStepFile` sans `return` → UB → crash SIGILL dans le pilote GL. + double refresh de la vue 3D évité (crashait certains pilotes). Return ajouté, refresh unique.

Suite : contrainte d'assemblage (mate/align), highlight persistant de la sélection, positionnement 3D interactif (gizmo).

## 2026-07-08 (suite 4) — 3D : pick HiDPI, surbrillance, apparence, autocomplétion

- **Pick 3D en HiDPI** (retour Lex, écran KDE ×1.4) : OCCT rend en pixels physiques, Qt donne les clics en pixels logiques → MoveTo cherchait à côté (« nothing under cursor »). `devicePos()` convertit via le rapport taille-fenêtre-OCCT / taille-widget-logique (robuste à tout facteur). Survol + clic + orbite + pan passent par là.
- **Surbrillance de sélection** : couleurs vives distinctes — cyan au survol, orange à la sélection (le gris par défaut était invisible sur une pièce métal ombrée).
- **Couleur + transparence des solides** : champ `transparency` sur SolidEntity (persisté) ; la vue OCCT applique la couleur résolue de l'entité + la transparence ; commande **TRANSPARENCY** (alias TRANS, %) ; le panneau Propriétés édite `transparency` et `component` (le blob BREP base64 est masqué) ; les éditions de couleur/transparence rafraîchissent la 3D. Validé : pièce à 40 % laisse voir l'intérieur.
- **Autocomplétion** : `CommandProcessor::commandNames()` alimente un QCompleter (MatchContains, insensible à la casse) dans la barre de commande — taper « 3D » liste MOVE3D/ROTATE3D/FILLET3D, etc. Découvrabilité des commandes (dont 3D) réglée.

## 2026-07-08 (suite 5) — Menu Assembly + Push/Pull (extruder une face)

- **Menu contextuel du panneau Assembly** (clic droit) : Propriétés, Couleur…, Transparence…, Renommer le composant…, Supprimer — chaque action en transaction annulable, répercutée en 2D et 3D.
- **Malentendu commande éclairci** : `EXTRUDE` = profil 2D fermé → solide (sketch → solide), PAS l'extrusion d'une face de solide. D'où « rien ne se produit » quand Lex sélectionnait une face 3D. La sélection 3D (OCCT) n'alimente pas non plus le système de commandes 2D.
- **Push/Pull (le vrai « extruder une face »)** : clic droit sur une face sélectionnée dans la vue 3D → « Push/Pull face… » → distance. `solidops::pushPullFace` prisme la face selon sa normale sortante puis fusionne (>0, bossage) ou coupe (<0, poche) avec le solide parent. La vue OCCT retient la face + le solide propriétaire ; MainWindow applique en transaction et rafraîchit. Testé sur une boîte (1000 → bossage 1500 → poche 700).

## 2026-07-08 (suite 6) — Sketch sur face + plan de travail généralisé

- **Malentendu EXTRUDE** rappelé : EXTRUDE = profil 2D fermé → solide (pas push/pull). Push/pull = clic droit sur une face.
- **Plan de travail généralisé** : `WorkPlane` passe d'un simple `zOffset` à un repère orthonormé (origine + normale + axe X). `to3d`, `extrudeWires` (prisme selon la normale), `revolveWires` (axe dans le plan) généralisés ; défaut = plan XY monde (2D inchangé). Stockage par document via `documentWorkplane(doc)`.
- **Sketch sur face** : `solidops::planeFromFace` extrait le plan d'une face PLANE (origine/normale/axe X, sens sortant). Clic droit sur une face en 3D → « Sketch on this face » fixe le plan de travail sur cette face et bascule en 2D ; le profil 2D dessiné y est placé et EXTRUDE construit perpendiculairement à la face. `WORKPLANE XY` réinitialise.
- **Surbrillance** : correction `LocalSelected`/`LocalDynamic` (les faces sont des sous-formes) → la face sélectionnée est bien en orange. **Ctrl+Z/Ctrl+Y** câblés. Verbe IPC `pick3d` pour piloter/vérifier la sélection 3D.
- Tests : extrude sur plan non-XY (cylindre selon +X, bbox vérifiée), planeFromFace sur les faces d'une boîte, push/pull (bossage/poche). 101 cas verts, ASan propre.

**Limite connue (à améliorer pour l'ergonomie Fusion)** : on dessine le profil dans le canevas 2D abstrait (pas visuellement posé sur la face à l'écran) ; la vue ne se réoriente pas encore face à la face. Prochain pas : environnement de sketch réorienté + silhouette de la face en référence.

## 2026-07-08 (suite 7) — Sketch-sur-face v2 + prépa nuit autonome

- **Sketch-sur-face v2** : `solidops::faceOutline2d` projette les arêtes de la face (contour + trous + découpes) dans le repère 2D du plan de sketch ; le canevas les dessine en bleu pointillé (référence) et zoome dessus, et masque les placeholders `[3D LxlxH]` pendant le sketch. On dessine enfin SUR la face réelle. Verbe IPC `sketchface` (extraction de `beginSketchOnFace`) pour piloter/vérifier. Validé sur MOTOR_FIX (trous + découpe + arête courbe visibles ; cercle dessiné au bon endroit).
- **Prépa nuit** : `docs/FUSION_GAP.md` = liste priorisée de l'écart avec Fusion (sketch/features/assemblage/nav/paramétrique), taguée [AUTO]/[GUI]. `scripts/overnight-workflow.mjs` = workflow qui implémente ces features EN SÉQUENCE (jamais en parallèle : arbre partagé), chaque agent build+test Catch2+commit-si-vert/revert-si-rouge. REPRISE.md a une section « TRAVAIL NOCTURNE AUTONOME » en tête. Lex dort ~5-6 h et m'a autorisé à poursuivre seul : lancer le workflow après compaction, relancer par lots jusqu'à son retour.

## 2026-07-09 — Nuit autonome : 21 features vers l'ergonomie Fusion

Lex a dormi ~5-6 h en m'autorisant à travailler seul via des workflows
autonomes (build + test Catch2 + commit-si-vert / revert-si-rouge, en séquence).
Bilan : **suite 814/102 → 1256 assertions / 172 cas**, arbre propre, 21 features
commitées atomiquement.

**Incident nuit 1 (récupéré).** Le premier lancement du workflow a échoué :
surcharge serveur API 529 sur les 4 premières features, ET un agent mort a
laissé l'arbre sale, ce qui a fait échouer en cascade tout le reste (l'ancienne
règle « abort si arbre sale »). Rien de commité. Correctifs :
- Récupéré `hole-feature` du stash, fini/testé/commité à la main (`866e93d`).
- **Piège OCCT gravé** : `BRepPrimAPI_Make*::IsDone()` n'est pas fiable tant que
  `.Shape()` n'a pas forcé le build → toujours construire la forme et
  null-checker (mordu sur le cylindre du trou).
- **Workflow durci** (`dd5e3e0`) : chaque agent fait `git reset --hard HEAD` au
  démarrage (le commité est sauf) au lieu d'abandonner → plus de cascade ; +
  idempotence (grep avant d'implémenter) pour relancer sans doublon.

**Lot 1 (8 features)** : EXTRUDE modes New/Join/Cut/Symmetric ; SHELL (coque
BRepOffsetAPI_MakeThickSolid) ; congé/chanfrein d'arêtes CHOISIES (filletEdges/
chamferEdges) ; MATE d'assemblage (2 faces planes → gp_Trsf) ; export STL
(binaire/ASCII, CLI `export .stl`) ; mesure 3D (minDistance/MEASURE3D) ; snap
sur la référence de face en sketch ; table de paramètres PARAM (évaluateur
d'expressions, persistant).

**Lot 2 (7 features)** : détection d'interférences (CLASH/checkAllInterferences)
; patterns 3D (PATTERN3D rect, PATTERNPOLAR3D) ; SWEEP + LOFT ; snaps NEAREST/
NODE/TANGENT ; DRAFT (dépouille de faces) ; SECTION (plan de coupe → profil +
aire) ; **fondations de l'historique paramétrique** (`FeatureTree` : Sketch +
Extrude rejouables, régénération éditable — purement additif, nouveaux fichiers,
n'altère pas SolidEntity/.vkd).

**Lot 3 (6 features)** : export OBJ (maillage, à côté du STL) ; cœur des vues
normalisées (directions Top/Front/Right/Iso + alignToFaceDir) ; **persistance du
plan de travail** dans le .vkd + purge des snaps de sketch obsolètes ; dette 2D
(word-wrap MTEXT par largeur de colonne + suffixe DIMSTYLE dimpost) ;
**mise en plan HLR** (`render::projectToDrawing` + commande MAKEVIEW : projette
un solide en vues 2D par élimination des lignes cachées) ; détection de régions
auto (arrangement planaire de courbes sécantes).

**Vérifié à la main (CLI) :** STL (box → 12 triangles, 684 o), OBJ (24 v / 12 f),
PATTERN3D (3×2 → 6 corps), CLASH (overlap 200 mm³ exact), SECTION (aire 100 mm²),
MAKEVIEW TOP (4 arêtes visibles, projection 20 u), FeatureTree additif confirmé.

**À valider VISUELLEMENT par Lex (non testable sans souris) :** placement/miroir
éventuel de la vue MAKEVIEW par rapport au modèle ; ressenti des commandes 3D
nouvelles dans la GUI ; toutes ces features sont pour l'instant pilotables au
clavier/CLI — l'intégration GUI fine (boutons, gizmos, ViewCube widget, vue de
sketch réorientée) reste à faire et demande son pilotage.

**Non fait exprès (demande le pilotage de Lex ou est purement GUI)** : solveur de
contraintes de sketch (H/V/parallèle/tangent…) et cotes pilotantes ; gizmo de
déplacement 3D à la souris ; matériaux/apparences réalistes ; câblage de
`FeatureTree` dans SolidEntity/.vkd (refonte du modèle de données). Voir
`docs/FUSION_GAP.md` (items cochés).

## 2026-07-09 (suite) — Revue + UX 3D souris façon Fusion + GPLv3

Retour de Lex : la souris doit servir à placer les features 3D (trou…), avec
aperçu coloré ajout/retrait comme Fusion ; il faut scinder/fusionner des
solides (plan OU surface courbe) ; et « ça manque de fini » → revue générale
avant correctifs, en vue d'une distribution open source.

**Revue (3 agents lecture seule)** : (a) pipelines 2D/3D disjoints — la vue 3D
n'alimentait aucune commande ; (b) FeatureTree fonctionnel mais déconnecté du
document ; (c) **libdxfrw est GPLv2+ → binaire distribué = GPL ; MIT/Apache
impossible avec le DXF lié**. Décisions Lex : **tout GPLv3**, UX 3D d'abord,
câbler le FeatureTree.

**UX 3D (fait à la main, `2b5fa35`)** : la vue 3D devient un périphérique
d'entrée de commandes. `Command::preview3d()` (fantôme TopoDS + effet
Add/Remove/Neutral) ; HOLE le fournit (cylindre ROUGE translucide qui suit le
curseur). OcctViewWidget::attach(doc, processor) ; au survol pendant un prompt
Point : la face plane survolée devient le plan de travail, le point de surface
OCCT est projeté en UV (`projectToPlane2d`/`planePoint3d`, nouveaux publics),
snappé sur les features réelles de la face (faceSnapPoints2d, ~10 px), envoyé
en pointerHint, et le fantôme suit. Clic = le point ; si la commande demande
ensuite une entité, le solide du même clic répond : **HOLE 6 T + UN clic sur
une face = trou percé perpendiculairement à cette face**. Fini trouvés en
passant : les commandes 3D tapées ne rafraîchissaient jamais la vue 3D
(corrigé : onInteraction + barre de commande) ; FitAll ne se fait plus qu'au
premier affichage (la caméra ne saute plus).

**Lot 4 (workflow séquentiel, 5/5 commités)** :
- `763c5cd` **SPLIT/COMBINE** : splitSolid (BRepAlgoAPI_Splitter, outil = plan
  OU face courbe OU solide), splitByPlane ; commande SPLIT [XY/XZ/YZ/Solid]
  (héritage layer/couleur/composant), COMBINE/FUSE n solides ; clic droit 3D
  « Split solid by this face's plane ».
- `644e8c7` **FeatureTree câblé** : nœuds BaseShape/Hole/Shell + setters ;
  JSON round-trip ; SolidEntity::features (deep-copy, sérialisé dans
  geomToJson → .vkd + undo) ; regenerateFeatures() ; HOLE et SHELL enregistrent
  leur historique.
- `e746bde` **Panneau Propriétés paramétrique** : « hole 1: diameter » etc.
  éditables → régénération + transaction (undo OK).
- `841b63f` **MOVE3D P** : déplacement par 2 points cliqués (UV du plan de
  travail → vrai gp_Vec 3D), forme dx/dy/dz intacte.
- `cef6ea0` **GPLv3** : LICENSE (texte intégral gnu.org), section licences du
  README (tableau des deps), .desktop portable (Exec=vikicad, Comment anglais
  + [fr]), cibles cmake --install (bin, .desktop, icône, MIME), CONTRIBUTING,
  CHANGELOG 0.1.0, CI GitHub Actions.

Vérifié CLI : SPLIT XY 4 → 2 pièces ; COMBINE → 1 solide ; trou paramétrique
persistant dans le .vkd (« features » présent) ; cmake --install stage les 5
fichiers. **Suite : 1506 assertions / 191 cas verts.**

Reste à valider par Lex en GUI : ressenti du fantôme rouge + snap au survol,
clic-unique HOLE, Split par clic droit, édition du diamètre dans Propriétés,
MOVE3D P à la souris. Backlog suivant (avec son pilotage) : solveur de
contraintes 2D, vue de sketch réorientée, gizmo 3D, ViewCube widget,
enregistrement EXTRUDE dans l'historique paramétrique.

## 2026-07-10 — Passe « professionnel » : causes racines + harnais d'auto-test

Retour dur (et juste) de Lex : push/pull → pièce disparue, Ctrl+Z toujours
mort, pas de menu Edit, déplacements à l'aveugle — « trouve des process pour
te tester, révise tout, ne me déçois plus ».

**Méthode changée : reproduire avant de corriger, vérifier sur la vraie GUI
avant de livrer.**

Causes racines trouvées par reproduction/lecture (pas par devinette) :
1. **Pièce disparue** : `pushPullFace` sur face COURBE → ok=true avec un
   compound VIDE (0 solide), commité tel quel. Fix double : refus des faces
   non planes + `booleanOp`/`requireSolid` refusent tout résultat sans solide
   (généralisé à tous les producteurs de SolidResult, commit 9ecb3b1).
2. **Ctrl+Z JAMAIS fonctionnel** : DEUX QShortcut sur Ctrl+Z (constructeur +
   shortcuts.json) → « ambiguous » Qt → aucun ne tire. Fix : menu **Edit**
   (Undo/Redo/Delete, un seul binding chacun) + loadShortcuts ignore les
   touches déjà prises par les menus.
3. **RPC exec ne rafraîchissait pas la 3D** (3 captures identiques au test
   live). Fix : sync3DView dans le chemin IPC.
4. **Trièdre X/Y/Z** qui suit le curseur pendant toute saisie de point (fini
   le déplacement à tâtons).

**Harnais d'auto-test GUI : `scripts/gui-smoke.sh`** (commit 7beff7d) — lance
la vraie GUI (systemd-run), la pilote par IPC, vérifie 40 assertions : compte
d'entités ET diffs de captures d'écran (UNDO doit restituer le rendu au pixel
près), split/combine, export STL. À exécuter AVANT toute livraison à Lex.
A aussi corrigé au passage la lecture des réponses IPC fragmentées du CLI.

**Durcissement systémique** (workflow 5 agents, tous verts) :
- `TransactionScope` RAII + soupape UNDO/REDO (une transaction fuitée tuait
  l'undo en silence — plus possible, commit d49ddbb).
- `requireSolid` sur toutes les ops solides + 3 tests dégénérés (9ecb3b1).
- Menus complets : menu **Solids**, **Help > About** (version + GPLv3),
  vérification de couverture commande↔menu (69fe91c).
- Chasse aux échecs silencieux : chaque action refusée dit pourquoi (9b4ecd7).

**État : 1620 assertions / 210 cas verts + gui-smoke 40/40.**

## 2026-07-10 (suite) — Push/pull de trou, clic droit, onglets, SKETCHES v1

- **Push/pull sur paroi de trou = rayon** (sémantique Fusion) via FeatureTree,
  refus si ça fermerait le trou. **Clic droit** picke désormais ce qui est
  sous le curseur (l'état de pick vidé par les rebuilds le rendait muet).
  Test-vérité : EXTRUDE négatif = extrusion vers le bas, PAS un trou (Cut/HOLE
  pour ça).
- **Onglets** : bandeau d'outils à onglets Draw/Modify/Annotate/Measure/
  Blocks/Solids/Views (une rangée, fini le foutoir sur 3 lignes) ; panneaux
  Layers/Properties/Assembly tabifiés à droite (9811b4f).
- **SKETCHES v1** (9168b0c) — la spec de Lex, à la lettre : sketch = objet
  léger {nom, plan, entités}, registre persisté dans le .vkd ; commande
  SKETCH New/Open/Close/List ; sketch-sur-face auto-capturé ; groupe
  « Sketches » dans le browser (ouvrir/renommer/supprimer, sélection =
  entités surlignées) ; étiquette de sketch ouvert dans la barre d'état ;
  **les profils d'un sketch SURVIVENT à l'extrusion** (réutilisables — le
  smoke extrude deux fois le même profil), les dessins hors sketch sont
  consommés comme avant ; **AUCUNE dépendance solide→sketch** (éditer un
  sketch ne régénère jamais un solide existant — décision de conception
  explicite de Lex, gravée ici).
- État : **1703 assertions / 217 cas + gui-smoke 55/55** (scénario sketch
  inclus), vérifié indépendamment après le workflow.

## 2026-07-11 — Sketch UX round 2, refonte souris, ergonomie de sélection

Trois vagues de retours terrain de Lex, toutes traitées par cause racine,
chacune validée suite complète + gui-smoke avant livraison :

**Sketch UX (8094397, e04b40b)** : repère de face DÉTERMINISTE aligné monde
(pln.XAxis() d'OCCT était arbitraire → le « désalignement 90° ») ; icône UCS
X/Y dans le canevas ; bouton vert « ✓ Finish sketch » (coin du bandeau) avec
retour 3D robuste ; ISOLATION du sketch sur face (le dessin modèle n'est plus
superposé au plan de la face — c'était l'autre moitié du « désalignement ») ;
sketches VISIBLES en 3D (fil bleu clair sur leur plan).

**Bug tokenizer tueur (4ee9c9f)** : le nom auto des face-sketches contenait
des espaces → coupé en tokens → tous nommés « Sketch » → le 2e échouait en
silence → pas d'isolation. Noms mono-token (FaceSketch-N) + abandon visible.

**LA trouvaille de Lex : Espace tapait un espace (4ee9c9f)** : QCompleter
route les touches du popup DIRECTEMENT au line edit en contournant notre
event filter → avec le popup omniprésent, Espace≠Enter → toutes les commandes
semblaient « bloquées ». Espace soumet depuis le popup désormais. Même bug
famille : Enter détourné par la ligne auto-surlignée du popup (« l » lançait
une commande au hasard) → index nettoyé sur textEdited (pas textChanged, qui
casse la surbrillance des flèches — corrigé le 11 aussi).

**Refonte souris (1f04220)** : gauche = sélection + BOÎTE de sélection au
drag (AIS_RubberBand + SelectRectangle) ; orbite = drag DROIT, menu = clic
droit court (menu Qt supprimé au press, synthétisé au release) ; milieu =
pan ; le tout remappable (Preferences). Courbes de sketch = objets AIS
individuels pickables (EntityId) → EXTRUDE clique le cercle en 3D et utilise
LE PLAN DU SKETCH (sémantique Fusion) ; aperçu live d'extrusion bleu/rouge ;
menu d'options aux prompts Keyword (clic droit → [New/Join/Cut/…]).

**Ergonomie de sélection (8d1091b)** : courbes en mode WIRE (priorité OCCT >
faces → le survol allume LA COURBE posée sur une face) ; Alt+clic = résolveur
de conflits (liste en profondeur de tout ce qui est sous le curseur) ; menu
clic droit en ARBORESCENCE (Hole ▸ / Face ▸ / Edges ▸ / Move ▸ / Select ▸) ;
surbrillance contrastée du popup ; curseur flèche épinglé sur menus/onglets.

**État en pause : 1717 assertions / 218 cas + gui-smoke 55/55 verts.**
Pause décidée par Lex le 2026-07-11 ; reprise la semaine suivante.

## 2026-07-12 — FEATEDIT : l'éditeur de features headless

Le panneau Properties savait éditer les paramètres de trou/coque ; les agents
non. **FEATEDIT** (core/cmd/CommandsFeatEdit.cpp) expose EXACTEMENT le même
chemin (featureparams::set + SolidEntity::regenerateFeatures, UNE transaction)
en grammaire headless « paramètres avant sélection, id en dernier » :
`FEATEDIT <param> <valeur> <nodeIndex> <solidId>` avec param ∈ {diameter,
depth, centerx, centery, height, thickness} (centerx/centery → « center x/y »,
signés : DÉPLACENT le perçage) ; `FEATEDIT LIST <solidId>` imprime chaque
paramètre éditable « hole 1: diameter = 4.0 » (une décimale, contrat INSPECT).
Erreurs claires : id non-solide, solide sans historique, param/nœud
incompatible (rollback, rien sur la pile d'undo), régénération échouée.
Tests tests/test_featedit.cpp (LIST, volume après élargissement, déplacement
du perçage sondé par interferenceVolume, UNDO/REDO, tous les chemins
d'erreur) ; harnais gui-smoke étendu (LIST via IPC, élargissement visible à
l'écran, UNDO restaure le rendu, mismatch rapporté). Suite : 1951 assertions /
227 cas ; gui-smoke 72/72 verts.

## 2026-07-12 — Parité agent complète : les mains ET les yeux

Question de Lex : « le CLI a suivi tout du long, ou un agent va galérer ? »
Audit : 7 opérations étaient GUI-only (push/pull, congé/chanfrein d'arêtes,
coque à faces ouvertes, split par face, édition de trou, MATE, DRAFT). Deux
lots ont fermé l'écart, chacun validé suite + gui-smoke.

**Parité d'ACTION (9d13d2e→86ae2b6)** : `SubShape.{h,cpp}` (faceAt/edgeAt par
ordre TopExp déterministe, 0-based) ; `INSPECT` (liste faces/arêtes : indice,
type, aire/longueur, centroïde — le vocabulaire d'adressage, et la GUI annonce
l'indice au pick) ; `FEATEDIT` (éditer diamètre/centre/profondeur d'un trou en
headless) ; `PUSHPULL`/`SHELLOPEN`/`SPLITFACE` (par indice de face, courbe
incluse) ; `FILLETEDGES`/`CHAMFEREDGES`/`MATE`/`DRAFT` (par indices).
`docs/AGENT.md` = guide complet, chaque exemple exécuté avant commit.

**Parité de PERCEPTION (ad8a835, 88b60aa)** — l'exigence de Lex « comprendre
et VOIR, sinon useless » : `DESCRIBE`/`DESC` (résumé calculé lisible : volume,
aire, bbox, centroïde, historique des features, sketches, calques) +
`query describe` (MÊME données en JSON machine, numériques, AUCUN brep base64)
câblé CLI hors-ligne ET IPC ; verbe IPC `viewdir TOP|FRONT|ISO…` → la boucle
visuelle `view3d on → viewdir → screenshot → diff` ; `LIST` enrichi
(volume/bbox/features sur les solides).

**Preuve ultime — l'agent aveugle** : la sanity a joué un agent n'ayant QUE
`docs/AGENT.md` + le CLI : boîte 30×20×10 → trou d5 → `query describe`
volume=5803.6505 (exact) → `FEATEDIT diameter 8` → volume=5497.3454 (exact) →
export STEP (ISO-10303 valide). Verdict : le guide seul a suffi, zéro lecture
de source, zéro tâtonnement.

**État : 2182 assertions / 243 cas + gui-smoke 124 checks, tout vert.**

## 2026-07-12 (suite) — Export GUI, résolveur visuel, aperçu rayon X

- **File > Export** (STEP/DXF/STL/OBJ) : les moteurs existaient depuis M3/M7,
  le menu manquait ; verbe IPC `export` + 4 checks harnais (c263769). DWG =
  import seulement (écrire du DWG demande une licence ODA — DXF est la voie).
- **Résolveur de sélection VISUEL** : survoler une ligne de « Select ▸ » /
  Alt+clic surligne l'élément dans la vue (ae8952c) ; puis **aperçu RAYON X**
  (ad45f5b) : le candidat survolé est un fantôme AIS dans la couche
  Graphic3d_ZLayerId_Topmost — il brille orange AU TRAVERS de tous les
  solides (assemblages denses = candidats occlus, une surbrillance avec test
  de profondeur était invisible). L'aperçu ne touche plus jamais la vraie
  sélection (objet indépendant).
- État : **2182 assertions / 243 cas + gui-smoke 124 checks verts.**
- Prochaine étape convenue avec Lex : TOUR de l'appli + BRAINSTORM d'une
  nouvelle fonctionnalité (session suivante, après compaction).

## 2026-07-16 — Brainstorm : le prochain chantier est le PCB CAM

Backup du repo (source + .git, sans build/) sur la clé TRANSCEND :
`vikicad-backup-20260716-2020.tar.gz` — seule copie hors machine tant que
GitHub reste injoignable (Ethernet IPv6-only).

**Brainstorm avec Lex (le rituel convenu avant tout gros chantier).** Son
idée : lire et éditer les Gerbers et fichiers PCB sans repasser par un gros
EDA (Altium & co). Analyse : le Gerber RS-274X est du dessin 2D vectoriel —
tracés à épaisseur = polylignes à bulge, flashes = inserts de blocs, régions
= hatches, couches = calques, X2 = JSON préservé. Ajustement quasi parfait
avec le moteur existant, et trou réel dans l'écosystème (aucun ÉDITEUR
Gerber libre ; gerbv/GerbView = visualisation seulement).

**Décisions (AskUserQuestion)** : périmètre = CAM Gerber/Excellon (PAS d'EDA
complet — nets/routage/DRC = territoire KiCad) ; schémas = chantier SUIVANT
(scope pré-réfléchi : blocs intelligents + WIRE + refs auto) ; goldens = les
kits Gerber réels de Lex (il les fournit) + synthétiques commités + diff
d'images contre gerbv en renderer de référence.

Plan complet : **docs/PCB_CAM.md** (mapping, 3 défis — polarité LPC,
regénération de la table d'apertures à l'export, perf des cartes denses —
phases G1 import/rendu, G2 ergonomie CAM, G3 édition/export/panélisation/
pont DXF↔Gerber). En attente : kits de Lex dans ~/computer/pcb-ref/ et
`sudo apt install gerbv`.

## 2026-07-16 — Clôture G1 : import Gerber/Excellon + rendu fidèle, durci post-revue ✅

**Ce qui est construit (commits 0aab919, 5f3f160, 697b68a + durcissement).**
Parseur RS-274X complet pour le dialecte Altium (FSLAX25/MOIN, macros %AM
prim 21+1 avec rotation origine-macro, G36/G37, arcs G74/G75, LPC/LPD,
attributs X2 nus ET en commentaires `G04 #@!`), parseur Excellon (INCH/
METRIC, TZ/LZ, sections PLATED/NON_PLATED, slots), « ouvrir un kit » =
répertoire → un calque nommé/coloré par fichier, ordre de peinture cuivre→
silk→outline→perçages, TOUT le kit dans UNE transaction. Rendu LPC par
pixmap ARGB par calque (CompositionMode_Clear intra-calque). CLI `import`
et GUI (File > Open Gerber kit + IPC `open`).

**Durcissement post-revue adversariale (cette session) :**
- *Élection de contour corrigée (major)* : le GKO de S5M0PCBB est une ZONE
  keepout pleine (rectangle G36 sur l'antenne ESP32) — elle était élue
  « Outline » et rendue en pavé magenta opaque au-dessus de tout. Nouvelle
  élection : candidats GKO > GM1 > GM13, mais un candidat n'est plausible
  que s'il contient au moins un TRAIT (un fichier régions-seules = zone
  pleine) et s'il s'étend sur ≥ 60 % de la carte sur au moins un axe.
  Résultat vérifié : PCBB → Outline = GM1 (le profil du bord haut, encoches
  + languettes), Keepout = GKO peint SOUS le cuivre (rang 5, violet) ;
  PCBA → Outline = GM13 (GKO/GM1 vides). Fini le pavé qui masque l'antenne,
  fini le kit sans contour.
- *D00 rejeté* : `X..Y..D00*` était silencieusement dessiné comme un D01
  (trou du test `dnum > 3`). Désormais erreur explicite ligne N.
- *Un fichier cassé ne tue plus le kit* : en import RÉPERTOIRE, un fichier
  sniffé fab mais imparsable est skippé avec warning ; en import FICHIER
  explicite, l'erreur reste dure.
- *GUI : un fichier Gerber/Excellon SEUL s'ouvre* (sniff de contenu dans
  MainWindow::loadFile → chemin kit) — parité avec le CLI ; vérifié par
  gui-smoke (« open lone .GTL »).
- *scripts/gerber-ref-diff.sh* : diff visuel par couche contre gerbv
  (export PNG CLI, masques d'encre binarisés + dhash gui-smoke). SKIP
  propre tant que gerbv n'est pas installé ; seuils premier-run à
  calibrer à la première exécution réelle.

**Pièges rencontrés** : le « contour » d'une carte réelle peut n'être qu'un
fragment (PCBB : GM1 ne dessine QUE le bord haut façonné, 68 % de la largeur,
24 % de la hauteur) — d'où le critère un-seul-axe ; et un GKO non vide n'est
pas forcément un contour (keepout d'antenne) — d'où le critère « au moins un
trait ». Détails dans LESSONS.md.

**État des tests : 3807 assertions / 274 cas ctest TOUS verts (2182/243 au
départ du chantier) ; gui-smoke 149 checks verts (124 au départ).** Captures
de validation : scratchpad `g1-fix/kitA-after.png`, `g1-fix/kitB-after.png`.

**Dette assumée (voir PCB_CAM.md)** : %SR non-identité refusé, G85 slots
Gerber absents du dialecte, exposure 0 macro refusée, tracé rectangle =
approximation ronde (aperture R en D01), import kit tout-ou-rien seulement
assoupli côté répertoire. En attente Lex : `sudo apt install gerbv`.

## 2026-07-17 — Calibration gerber-ref-diff : le bug d'ordre LPC attrapé par le renderer de référence ✅

Premier run RÉEL de `scripts/gerber-ref-diff.sh` (gerbv enfin installé) :
28 FAIL / 32 couches. Une matinée d'enquête, TROIS causes — et une victoire
du process, parce que chacune a été prouvée avant d'être « corrigée » :

- **(A) Le « bug » d'ordre de peinture LPC… n'existait pas.** Le diff
  montrait ~27 % d'écart d'encre sur les couches cuivre, symptôme parfait
  d'une composition en 2 passes (tous les LPD puis tous les LPC). Golden
  synthétique `lpc_redraw.gbr` (plan → clear → piste re-dessinée → 2e clear
  → pastille flashée), sondes de pixels, et même INJECTION volontaire du
  bug hypothétique : la composition suit déjà strictement l'ordre du
  document, et les sondes détectent bien le bug quand on le crée. Le check
  reste commité en verrou de régression (gui-smoke, phase `lpc:`).
- **(B) Le vrai coupable : les décorations du canvas.** Glyphe UCS +
  réticule gonflaient la bbox d'encre des captures → crop faussé → toutes
  les couches divergeaient. D'où `screenshot PATH clean` (IPC
  `"overlays": false`) : rendu du document SEUL, partagé avec le chemin de
  peinture interactif. Le flag vaut aussi en vue 3D (capture 2D par
  définition) et l'IPC `open` remonte désormais les warnings d'import.
- **(C) Gerber valide mais VIDE** (PCBA GKO/GM1 : en-tête + M02) : refusé à
  l'ouverture avant, maintenant document vide + warning « valid but
  empty » (comportement gerbv) ; vide-vs-vide = PASS dans le diff.

Bonus tiré des chiffres : les perçages Excellon se rendaient en ANNEAUX
(cercles cosmétiques) vs disques PLEINS chez gerbv — dhash .TXT 58/104.
`CircleEntity` se remplit quand l'entité porte le tag `plated` → 11/40.

**Résultat : 32/32 couches VERTES**, seuils calibrés sur les maxima réels
+30 % (dhash ≤ 170 — max observé 132, halo AA du cuivre PCBB.GBL vérifié à
l'œil — ; ink-delta ≤ 3 pts). Le script (~12 s) tourne maintenant en stage
final OPTIONNEL de gui-smoke : SKIP silencieux sans gerbv/kits, FAIL sinon.
La parité visuelle avec gerbv est sous harnais permanent.

**État des tests : 3833 assertions / 275 cas ctest verts ; gui-smoke
157 checks verts (dont le stage refdiff).**

## 2026-07-17 — G2 : la pile de couches façon CAM (alpha, rang, rôle, BOARDVIEW) ✅

Quatre briques, toutes servies par le CommandProcessor unique (parité
CLI/IPC/GUI vérifiée dans les deux canaux) :

- **Alpha par calque** (`Layer.alpha` 0-100, défaut opaque) : le calque
  translucide passe par le composite ARGB déjà utilisé pour le LPC,
  opacité appliquée AU BLIT — le calque fond comme UNE surface, les
  recouvrements internes (piste sur piste) ne s'assombrissent pas.
  `LAYER <nom> ALPHA n`, colonne Alpha du LayerPanel, persisté .vkd
  (colonnes ajoutées par ALTER TABLE toléré + SELECT à repli pour les
  fichiers pré-G2) et exposé dans `query layers`.
- **Ordre de dessin générique** (`Layer.rank`, plus petit = peint d'abord) :
  stable-sort des entités par rang de calque au rendu — égalité de rang =
  ordre du document, donc TOUT document pré-G2 (rangs 0 partout) rend
  bit-identique (verrouillé par les hashes gui-smoke et refdiff 32/32).
  L'importeur de kit grave désormais ses rangs sur les calques ; UP/DOWN
  matérialise les rangs 0..n-1 puis échange (▲▼ dans le panneau).
- **Rôle Gerber réassignable** (`Layer.gerberRole`) : posé par l'importeur,
  réassignable par `LAYER <nom> ROLE <r>` / menu contextuel — recolore à la
  palette du rôle et re-range (Outline magenta rang 90 : l'échappatoire de
  l'élection de contour, dette G1 soldée).
- **`BOARDVIEW TOP|BOTTOM|ALL`** (+ View > Board view) : TOP atténue le côté
  bottom à 25 % (perçage/contour toujours pleins), BOTTOM symétrique ET vue
  miroir X **au niveau Camera2d** (worldToScreen/screenToWorld/panPixels) —
  picking, snaps et zoom marchent tels quels, la sérigraphie bottom se lit
  à l'endroit, exactement la « vue côté soudure » CAM. Le miroir est un
  état de vue (jamais persisté, remis à zéro à l'ouverture d'un document).
  ALL restaure tout — capture clean identique À L'OCTET à l'initiale.

Leçon de mesure au passage : le comparateur d'encre en NIVEAUX DE GRIS de
gui-smoke est quasi aveugle au swap cuivre rouge↔bleu (luminances 108 vs
121 — sous le seuil de 16) : l'inversion de pile a été validée par sonde
RGB (pixel du plan : (229,57,53) → (61,126,255)), pas par le bp gris.

**État des tests : 3961 assertions / 285 cas ctest verts ; gui-smoke
173 checks verts (16 nouveaux `stack:`, refdiff 32/32 inchangé).**

## 2026-07-17 — G2 : mesurer sur les gerbers comme un outil CAM (MINDIST, snaps, cotes) ✅

- **Snaps pastilles** : `InsertEntity::snapPoints` offre désormais le point
  d'insertion en Endpoint ET en **Center** (l'origine de flash d'un bloc
  GBR-* est le centre de la pastille) ; le SnapEngine passe par la même
  méthode au lieu de son push manuel. Coter centre-à-centre marche avec le
  seul osnap Center. Traces larges (extrémités/milieux), arcs à bulge
  (centres) et perçages (centres) étaient déjà couverts — verrouillés par
  deux nouveaux tests [snap][gerber].
- **MINDIST** (alias MD, CommandProcessor unique donc parité CLI/IPC/GUI
  gratuite — vérifiée dans les deux canaux) : distance minimale BORD À
  BORD, sémantique matière. Noyau core/edit/MinDist.cpp : chaque entité
  devient une soupe capsules (segment + rayon — trace large = width/2) /
  disques (cercle = rayon) / polygones remplis (empreinte réelle des blocs
  GBR-* via la transformation d'insert, anneaux even-odd des pours), en
  réutilisant l'aplatissement du renderer (buildPrimitives, strokes filled
  → anneaux, width → rayon de capsule). Ce qui ne se réduit pas à des
  strokes (texte...) replie sur sa bbox et la sortie LE DIT (`method:
  "bbox"` + note). Recouvrement/contact → 0 + `overlap:true`, containment
  pur détecté (perçage entièrement dans sa pastille). Sortie : ligne
  humaine, points les plus proches, trailer JSON compact (`mindist.mm/pa/
  pb/method/overlap`, mm pleine précision) documenté dans AGENT.md §7b.
- **Ligne témoin** : overlay transitoire posé dans le CommandContext
  (ligne pointillée + tics aux deux points), dessiné par le canvas jusqu'à
  la PROCHAINE commande (le processor le purge au démarrage suivant) ;
  jamais dans contentImage() donc les captures clean restent géométrie
  pure.
- **Valeurs à la main** : traces parallèles 2.0−0.25−0.15 = 1.6 ; trace vs
  perçage 5−1−0.2 = 3.8 ; pastilles rect 2×1 à 5 mm = 3.0 ; perçages réels
  du kit A : |c1−c2|−r1−r2 = 7.510364123916017 mm reproduit à 1e-9 près
  (unitaire + gui-smoke recoupent la formule depuis les données query).
- **Cotes sur kit réel** : DIMALIGNED centre-pastille → centre-pastille
  vérifié offline (entité dimension avec a/b exacts) et en live : bloc
  gui-smoke `measure:` (12 checks) — MINDIST perçage-perçage == formule,
  témoin visible au canvas, cote posée (+1 entité, capture clean stable à
  0 pixel changé), UNDO restaure le rendu initial pixel pour pixel. Au
  passage : img_cmp exporte le compte BRUT de pixels changés
  (img_px_changed) — les bp planchonnent à zéro sur une cote fine, le
  canvas 2D étant déterministe le compte brut est fiable.

**État des tests : 4046 assertions / 299 cas ctest verts ; gui-smoke
185 checks verts (12 nouveaux `measure:`, refdiff 32/32 inchangé).**

## 2026-07-17 — G2 : l'inspection — chaque objet gerber sait CE QU'IL EST ✅

Troisième lot G2. Une pastille cliquée doit se raconter ; un agent doit
pouvoir lire la table d'apertures et le rapport de perçage sans rouvrir le
fichier source. Le tout pensé fondation G3 : l'export RS-274X/Excellon
régénérera ses tables depuis ces métadonnées.

- **Tags d'entités** : l'importeur Gerber pose `dcode:N` sur tout objet
  peint par une aperture (traces coalescées, arcs, flashes ; les régions
  G36/G37 n'ont pas d'aperture → pas de tag), l'importeur Excellon pose
  `tool:"Tn"` à côté du `plated` existant. Voyage via l'extra JSON — undo,
  .vkd et queries gratuits.
- **Tables persistées au calque** : nouveau `Layer.camMeta` (QJsonObject),
  colonne `cam_meta` dans .vkd (ALTER TABLE toléré + SELECT à 3 étages :
  +cam_meta → +alpha/rank/role → legacy — les fichiers d'HIER, avec
  colonnes G2 mais sans cam_meta, gardent leur pile). Gerber :
  `{"apertures":{"Dnn":{shape, params mm, macro?, hole?, desc, usage}}}` ;
  Excellon : `{"tools":{"Tn":{dia, plated, usage}}}`. Visible en `cam`
  dans query layers (seulement si non vide).
- **AMPARAMS** : Altium émet `G04:AMPARAMS|DCode=15|XSize=23.62mil|...|
  Shape=RoundedRectangle|` avant chaque %AM — la vérité niveau designer.
  Capturé au parse (brut, par D-code), converti en desc lisible :
  « RoundedRect 0.600x0.900 r=0.054 rot 270deg » (l'exemple du brief,
  reproduit verbatim sur le kit réel). Sans AMPARAMS : « Macro NOM ~WxH »
  (bbox des primitives). C/R/O/P décrits depuis leurs params mm.
- **APERTURES [calque]** (APER) : table alignée D-code/desc/usages + trailer
  JSON (contrat MINDIST : dernier message commençant par `{`). Test kits
  réels : {usage>0} == « Used DCodes » du .REP, couche par couche, sur les
  deux kits — y compris l'égalité vide du GKO keepout de PCBB (un fichier
  100 % régions ne définit aucune aperture, et le .REP le dit pareil).
- **DRILLREPORT** (DR) : table par diamètre+platage sur les cercles VIVANTS
  (un ERASE se voit au rapport suivant), outils et calques listés par
  rangée, trailer JSON. Golden : chaque outil des .DRR retrouvé (platage,
  ø ±0.01, compte EXACT), totaux 182 (PCBA) / 330 (PCBB).
- **Inspecteur PropertiesPanel** : section lecture seule au-dessus de la
  géométrie — `gerber D-code`, `gerber aperture` (desc de la table du
  calque), `gerber polarity` (dark / clear LPC), `drill tool` + `drill
  plating`, `layer role`. Nouvelle commande générique **SELECT [ids]**
  (SEL, vide = clear) : le pickfirst headless (MINDIST pré-sélectionné
  vérifié en test) ET le moyen de piloter le panneau par IPC ; `query ui`
  expose `propRows` (les rangées visibles du QTableWidget) — le panneau
  est vérifié par gui-smoke sur le vrai widget, pas sur une maquette.
- **Dette G1 soldée — couche « 0 »** : un kit importé dans un document
  VIERGE (l'état des trois canaux d'open) supprime la couche « 0 » du
  constructeur (calque courant → première couche du kit) ;
  NativeStore::load ne la ressuscite pas quand le fichier n'a pas de ligne
  id 0 (Document::dropEmptyLayerZero, refus si une entité ou un bloc la
  référence). Un document NON vierge garde sa « 0 » — flux DXF/2D intacts.

**État des tests : 4380 assertions / 307 cas ctest verts (8 nouveaux cas
test_cam_inspect dont 2 goldens kits) ; gui-smoke 200 checks verts (15
nouveaux `inspect:`), refdiff 32/32 inchangé.**

## 2026-07-17 — Clôture G2 ✅ : revue adversariale + fix MINDIST union

Passe de revue adversariale sur les 6 commits G2 (deux reviewers
indépendants, kits réels décodés à la main comme vérité) : DRILLREPORT ==
.DRR ligne à ligne, APERTURES == .REP couche par couche sur les DEUX kits,
MINDIST recoupé à 1e-9 sur perçages/pastilles/traces (rotations d'insert
vérifiées : 0.900176 pour le D15 rot 270, pas 0.600202),
alpha/rank/role persistants, .vkd pré-G2 rétro-compatibles. Deux défauts
réels trouvés dans core/edit/MinDist.cpp, le majeur corrigé aujourd'hui :

- **[majeur, corrigé] La parité even-odd inter-anneaux mentait sur les
  pastilles macro multi-anneaux.** Un RoundedRect Altium flashe 6 anneaux
  qui se RECOUVRENT (2 rects + 4 disques de coin) — sémantique d'UNION. Le
  test de containment de MINDIST appliquait la parité even-odd à travers
  TOUS les anneaux : un point couvert par 2 anneaux comptait « dehors »,
  et un objet 100 % enterré dans la pastille sortait « 0.196 mm (exact),
  overlap:false » (distance vers une couture INTERNE de l'union). Repro
  kit réel : cercle au centre du flash D15 #773 → maintenant
  `overlap:true, mm:0`. Fix : `insideUnion` (even-odd DANS un anneau,
  union ENTRE anneaux — exactement ce que peint le renderer SOLID qui
  remplit chaque anneau individuellement) + un point représentatif par
  ANNEAU côté invité. Test unitaire d'abord (pastille « plus » en 2 rects
  qui se croisent, rouge avant fix), les recouvrements partiels passaient
  déjà par les distances de frontière négatives.
- **[mineur, dette] Pastilles rondes = polygones inscrits** → clearance
  sur-estimée jusqu'à ~2 µm en oblique (non-conservateur). Documenté en
  dette PCB_CAM.md, à régler avec les empreintes exactes de l'export G3.
- **[mineur, corrigé] Overlay MINDIST illisible à l'échelle carte** : le
  pointillé rose cosmétique 1 px se noyait dans le cuivre. Halo sombre
  4 px sous les pointillés 1.8 px — lisible sans zoomer, toujours hors
  captures clean.
- **[mineur, acté] Snap Center sur le point d'insertion de TOUT insert**
  (pas seulement GBR-*) : changement de comportement assumé pour les
  dessins 2D à blocs, documenté (LESSONS + dette PCB_CAM).

**État des tests : 4384 assertions / 308 cas ctest verts (+1 cas union
multi-anneaux) ; gui-smoke 200 checks verts, refdiff 32/32 inchangé. G2
CLÔTURÉ — prochaine étape G3 (édition + export RS-274X/Excellon).**

## 2026-07-17 — G3 : les écrivains RS-274X + Excellon, la boucle CAM bouclée ✅

La phase entière tient en une journée parce que G2 avait préparé le
terrain : `dcode`/`tool`/`gpol`/`plated` sur les entités, tables
d'apertures/outils dans `Layer.camMeta`, macros %AM persistées. Détail
technique complet dans PCB_CAM.md (blocs « Fait 2026-07-17 ») ; commits
258f0ab → 7e860aa. L'essentiel :

- **GerberWriter** : dialecte moderne %FSLAX46Y46/%MOMM, LPD/LPC en suivant
  l'ORDRE de peinture, G36/G37 pour les régions, table d'apertures
  RÉGÉNÉRÉE (définition d'origine si le width est intact — un tracé à
  aperture RECT ressort en R —, `C,w` pour un width édité, %AM re-émis
  VERBATIM converti inch→mm, transformations d'insert repliées dans les
  params standard, fallback macro outline + warning) ;
- **ExcellonWriter** : M48/METRIC,TZ, sections ;TYPE=PLATED/NON_PLATED,
  coordonnées DÉCIMALES EXPLICITES (tranché par l'expérience gerbv — voir
  LESSONS), table d'outils régénérée (groupage 1e-4, diamètre pleine
  précision du premier cercle) ;
- **export kit** (rôle+côté → extension Altium, tous les Drill dans UN
  .TXT) sur les 3 canaux (CLI/IPC/GUI), export mono-calque, PLWIDTH /
  LAYER CURRENT, PANELIZE (grille du contenu fab, un undo), pont
  DXF↔Gerber dans les deux sens (width constant à travers LWPOLYLINE 43 ;
  polyligne fermée + ROLE Outline → .GKO propre) ;
- **LE test de vérité** : gerbv rend chaque couche exportée identique à
  l'originale sur les DEUX kits (28 couches Gerber + drills), re-import
  sémantique à 1e-3 mm, comptes par outil == .DRR.

## 2026-07-17 — Clôture G3 ✅ : revue adversariale de l'export + garde-fous

Deux reviewers indépendants ont attaqué l'étage d'export : ré-export des
kits par leurs soins, décodage des flux avec leurs PROPRES parseurs
FS/MO/G36/LP/AM (pire déviation 0.000000000 mm — l'invariant
orig_int(2:5 inch)×254 = exp_int(4:6 mm) tient partout), apertures 33/33 à
1e-5 mm, décodage MANUEL de lignes brutes confronté aux entités source,
multiset Excellon (position, ø, platage) identique à 1e-6 sur 182+330
trous. Géométrie : irréprochable. Mais la revue a trouvé UN vrai trou
d'usage et une grappe de pièges opérateur, tous corrigés le jour même,
test rouge d'abord (commits bafd02d, 4117ab6) :

- **[majeur] `Drill-NPTH` était inatteignable** : le token n'existait pas
  dans gerberRoleSpecs() et le mapping nom→rôle matchait `DRILL` avant
  `DRILL-NPTH` — le calque importé portait le rôle `Drill`, `LAYER n ROLE
  Drill-NPTH` répondait « unknown role », et un trou DESSINÉ par
  l'utilisateur sortait TOUJOURS en PLATED (trou de fixation métallisé
  chez le fabricant, sans aucun moyen d'exprimer l'intention NPTH). Les
  défauts NPTH de l'ExcellonWriter et de DRILLREPORT étaient du code mort
  — et DEUX tests existants codifiaient le bug (corrigés vers le
  comportement juste, pas affaiblis). Fix minimal dans GerberRole.cpp :
  spec ajoutée + NPTH testé avant le préfixe générique.
- **[mineurs, tous corrigés]** : %TF nu → forme commentaire Altium
  `G04 #@!` + aperture placeholder sur calque à régions seules (gerbv ne
  crie plus CRITICAL/RS-274D sur nos exports — silence total vérifié) ;
  CLI `export` extension inconnue → E_FORMAT (fini le DXF silencieux
  nommé « gerbers ») ; warning extension↔dialecte (Excellon dans .gbr,
  RS-274X dans .txt) ; échec dur mi-kit → les fichiers déjà écrits sont
  SUPPRIMÉS et nommés dans l'erreur ; PANELIZE plafonné (2 M de clones —
  100×100 sur kit A = 23 M refusé net).
- **[dette actée]** : le JEU de fichiers exporté du kit B diffère de
  l'original (GKO = Outline élu, keepout/pads/mech en mono-calque) —
  documenté PCB_CAM avec le réflexe `skippedLayers`.
- **Nouveau juge au harnais** : `scripts/gerber-export-diff.sh` — gerbv
  sur l'ORIGINAL vs gerbv sur l'EXPORTÉ, apparié par calque SOURCE (le
  `fileLayers` ajouté au JSON d'import CLI), seuils SERRÉS. 31/31 PASS
  (dhash ≤ 1/1024, encre 0), ~5 s, stage final de gui-smoke.

**État final : ctest 5142 assertions / 334 cas ; gui-smoke 224 checks
(~1 min 50) ; ref-diff 32/32 ; export-diff 31/31. G3 CLÔTURÉ.**

## 2026-07-17 — BILAN DU CHANTIER PCB CAM 🏁 (16→17 juillet, G1+G2+G3)

Décidé au brainstorm du 16, LIVRÉ le 17 au soir. VikiCAD est devenu ce
que le plan visait : un éditeur CAM de fichiers de fabrication — lire,
comprendre, mesurer, éditer, réexporter du RS-274X et de l'Excellon, sans
EDA, le trou réel de l'écosystème Linux (gerbv/GerbView ne font que
regarder).

**Chiffres avant/après** (avant = pause du 2026-07-11) :
| | avant | après |
|---|---|---|
| ctest | 2182 assert. / 243 cas | **5142 / 334** |
| gui-smoke | 124 checks | **224** |
| juges externes | — | ref-diff 32/32, export-diff 31/31 |
| formats | DXF/DWG/STEP/STL/OBJ/PDF | + **RS-274X, Excellon** (les 2 sens) |

**Les paris qui ont payé :**
- **La polarité LPC comme ORDRE DE PEINTURE** (le concept neuf de G1) :
  champ `sort` existant + composite ARGB par calque — le piège classique
  du deux-passes (darks puis clears) évité, verrouillé par les probes
  pixel de gui-smoke et un golden dédié (lpc_redraw).
- **gerbv comme juge de paix, nous HORS de la boucle** : d'abord notre
  rendu vs gerbv (G1, ref-diff — a attrapé un vrai bug d'ordre LPC), puis
  gerbv vs gerbv sur original/exporté (G3, export-diff) : l'écriture est
  validée par un renderer qu'on n'a pas écrit.
- **.DRR/.REP d'Altium comme vérités indépendantes** : APERTURES == .REP
  et DRILLREPORT == .DRR couche par couche sur les deux kits — des goldens
  que PERSONNE chez nous n'a fabriqués.
- **camMeta pensé pour l'export dès l'inspection G2** : ré-émettre la
  définition D'ORIGINE (apertures rect, %AM verbatim) a contourné
  élégamment la dette de rendu G1 « bouts ronds » — l'exporté est parfois
  plus fidèle que notre écran, et c'est le bon sens de fidélité.
- **Les revues adversariales par phase** : chacune a trouvé au moins un
  vrai bug (élection de contour G1, union even-odd MINDIST G2, rôle NPTH
  G3) — aucun n'aurait été vu par nos tests « sympathiques ».

**Ce qui reste (dette consolidée, tout documenté dans PCB_CAM.md)** :
flashes ronds tessellés (~2 µm MINDIST), %SR et G85/slots absents,
calques sans mapping kit exportés un à un (jeu de fichiers ≠ original sur
kit B), PANELIZE sans rails/mousebites, exposure 0 des macros refusée.
Rien de bloquant pour l'usage réel ; à réévaluer quand Lex s'en sert.

**Prochains chantiers en réserve** (REPRISE.md) : gizmo de drag direct,
EXTRUDE/REVOLVE dans FeatureTree, contraintes de sketch, release GPLv3,
et le chantier schémas pré-réfléchi au brainstorm.
