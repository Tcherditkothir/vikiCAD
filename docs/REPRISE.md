# REPRISE — PAUSE au 2026-07-11, reprise semaine du 14 juillet

Document de reprise. À lire AVANT toute action, avec DEVLOG.md (historique
complet) et LESSONS.md (pièges connus — dont les gros du 10-11 juillet).

## ⏸️ ÉTAT À LA PAUSE (décidée par Lex le 2026-07-11)

- **Arbre propre, dernier commit `88b60aa`.**
- **Suite : 2182 assertions / 243 cas verts. Harnais GUI : 124 checks verts.**
- **PARITÉ AGENT COMPLÈTE (2026-07-12)** : un agent headless a les mêmes mains
  ET les mêmes yeux que la souris. Actions manquantes ajoutées (INSPECT,
  FEATEDIT, PUSHPULL, SHELLOPEN, SPLITFACE, FILLETEDGES/CHAMFEREDGES, MATE,
  DRAFT — adressage par indice de face/arête déterministe). Perception :
  `DESCRIBE` + `query describe` (JSON calculé, sans brep), `viewdir` IPC pour
  la boucle capture, `LIST` enrichi. Guide : **`docs/AGENT.md`** (validé par
  un « agent aveugle » qui a tout réussi avec le seul guide). File > Export
  (STEP/DXF/STL/OBJ) + verbe IPC `export`.
- Depuis le 09 : 21 features de nuit autonome, passe « professionnel »
  (causes racines : Ctrl+Z ambigu jamais fonctionnel, booléens vides =
  pièce disparue, TransactionScope), sketches v1 (légers, sans dépendance
  solide→sketch — décision ferme de Lex), onglets outils+panneaux, refonte
  souris (gauche=sélection+boîte, droit=orbite/menu), ergonomie de sélection
  (priorité courbe>face au survol, Alt+clic résolveur, menu en arborescence).

## 👀 EN ATTENTE DE VALIDATION PAR LEX (dernier lot, non testable souris)

- Survol : une courbe de sketch posée sur une face s'allume ELLE.
- **Alt+clic** = liste des candidats sous le curseur ; idem « Select ▸ » au
  clic droit ; menu clic droit en arborescence (Hole ▸/Face ▸/Edges ▸/Move ▸).
- Popup d'autocomplétion : flèches → surbrillance bleue, Enter prend la ligne
  choisie, Espace = Enter même popup ouvert.
- Souris : boîte de sélection au drag gauche ; orbite au drag droit ; preview
  d'extrusion bleu/rouge ; curseur visible sur les boutons du haut.
- Flux sketch complet : face → sketch isolé aligné (icône X/Y) → ✓ Finish →
  retour 3D, sketch bleu visible → EXTRUDE en cliquant la courbe (plan du
  sketch pris automatiquement).

## 🔜 PROCHAINS CHANTIERS (ordre de valeur probable — CONFIRMER avec Lex)

1. **Gizmo de drag direct** (déplacer solides/trous à la souris — le trièdre
   curseur existe comme base visuelle).
2. **EXTRUDE/REVOLVE enregistrés dans FeatureTree** (seuls HOLE/SHELL ont un
   historique aujourd'hui) + édition via l'arbre.
3. **Contraintes de sketch + cotes pilotantes** — GROS, piloter avec Lex
   (il refuse les dépendances lourdes sketch→solide, PAS les contraintes
   internes au sketch).
4. **Release publique GPLv3** : build Release + .desktop pointé dessus, tag,
   hébergement (GitHub INACCESSIBLE depuis la machine — IPv6 only ; prévoir
   une alternative ou un transfert par Lex).
5. Moyens : MAKEVIEW placement à valider ; ~~MATE/DRAFT sans GUI~~ (FAIT
   2026-07-12 : FILLETEDGES/CHAMFEREDGES/MATE/DRAFT headless par index
   INSPECT, harnais GUI étendu) ; double-clic
   sketch 3D → l'ouvrir ; presse-papier/duplication de solides.

## ⚠️ Harnais GUI-live : `scripts/gui-smoke.sh` — À LANCER AVANT DE LIVRER

**Lancer `scripts/gui-smoke.sh` (tout-vert) avant de remettre quoi que ce
soit à Lex.** Il build si besoin, démarre la VRAIE GUI (unité systemd
`vikicad-gui`), attend le ping IPC, puis déroule un scénario 2D
(RECT/CIRCLE/MOVE + UNDO/REDO, compteurs+bounds vérifiés) et 3D (EXTRUDE,
HOLE avec comparaison de captures par image-hash, UNDO/REDO du rendu,
SPLIT/COMBINE, export STL avec contrôle d'en-tête). Table PASS/FAIL, code de
sortie non-zéro au moindre échec, arrêt garanti de l'unité GUI (trap).
Dépendances : bash + python3-PIL seulement. Détails : CONTRIBUTING.md.

## ✅ NUIT AUTONOME DU 2026-07-09 — FAITE (21 features, suite 814→1256 assert.)

La nuit autonome est terminée. 3 lots de workflow (`scripts/overnight-workflow
.mjs`, `overnight-batch2.mjs`, `overnight-batch3.mjs`) + un correctif manuel ont
livré **21 features** vers l'ergonomie Fusion, chacune build+test+commit
atomique. Détail complet : `docs/DEVLOG.md` (entrée 2026-07-09) ; état de l'écart
restant : `docs/FUSION_GAP.md` (bloc « FAIT » en tête). **Arbre propre, suite
verte : 1256 assertions / 172 cas.**

Livré (headless-testé) : Trou, Extrude modes, Shell, congé/chanfrein d'arêtes,
Mate, interférences, Pattern 3D, Sweep/Loft, Draft, Section, `FeatureTree`
(fondations paramétriques), STL+OBJ, Mesure 3D, snaps NEAREST/NODE/TANGENT,
snap sur réf de face, PARAM, word-wrap MTEXT+dimpost, persistance du plan de
travail, **mise en plan HLR (MAKEVIEW)**, cœur des vues normalisées.

**À FAIRE au réveil de Lex :**
1. Lui présenter le bilan et le laisser VALIDER en GUI (surtout : MAKEVIEW —
   placement/miroir de la vue ? ; ressenti des nouvelles commandes 3D).
2. Les prochains gros morceaux demandent SON pilotage (ne pas foncer seul) :
   solveur de contraintes de sketch + cotes pilotantes ; vue de sketch
   réorientée ; gizmo 3D souris ; câblage de `FeatureTree` dans SolidEntity/.vkd
   (refonte du modèle de données) ; ViewCube widget. Voir `FUSION_GAP.md`.

**Leçons d'orchestration gravées** (LESSONS.md) : reset-au-démarrage de chaque
agent (jamais « abort si arbre sale » → cascade) ; idempotence (grep avant
d'implémenter) ; piège `BRepPrimAPI::IsDone()` non fiable. Les 3 scripts de nuit
sont réutilisables/relançables (idempotents) pour de futurs lots.

## État global

- Plan M0→M8 livré, tags git `m0`…`m8`. **191 cas de tests verts / 1506 assertions**
  (dont la nuit autonome du 2026-07-09, +21 features 3D/paramétriques).
- Phase actuelle : **M6-usage** — Lex utilise VikiCAD sur ses vrais fichiers
  (clé USB TRANSCEND, `/dev/sda1`, monter via `udisksctl mount -b /dev/sda1`)
  et remonte les bugs un par un. C'est LA priorité : réactivité sur ses retours.
- Dernier commit : « Live preview for OFFSET, GEAR, ELLIPSE, SPLINE ».

## Chantiers TRAITÉS le 2026-07-08 (2 sessions), en attente de validation Lex

Session 1 (MTEXT + snaps + cotes) :
1. **Blocs de texte (MTEXT/TEXT)** : attachment 71, vAlign/lineSpacing,
   justification TEXT 72/73/11, décodage inline complet, export symétrique.
2. **Snaps dans les blocs** : snapQuery récurse dans les définitions.
3. **Cotes** : DIMSTYLE importé (×DIMSCALE), override 1 avec `<>`, styleScale.
4. **ZOOM W** (fenêtre par deux coins).

Session 2 (nouveau lot de retours) :
5. **Troncature MTEXT** (`Immeuble protég`) : dwg2dxf enveloppe les valeurs à
   254 o avec un CR/LF brut ; le lecteur re-colle maintenant la continuation
   (patch 0004 révisé). Texte complet.
6. **Menu snaps par type** : clic droit sur le bouton SNAP.
7. **Répéter la commande** : clic droit canvas (à vide) / Enter (en cours).
8. **Objets mal orientés** : (a) `EllipseEntity::transform` refait en affine
   correct (demi-diamètres conjugués) ; (b) ellipses à extrusion Z<0 → params
   réfléchis à l'import.
9. **Édition texte** : double-clic → dialogue ; commande `TEXTEDIT` (ED/DDEDIT).
10. **Panneau Layers** : colonnes triables + docks flottants agrandissables.
11. **Outil ENGRENAGES** 🎉 : `GEAR`/`SPURGEAR` — profil à développante +
    sticky note de conception (cotes + accouplement + raisonnement). Géométrie
    dans `core/geom/GearGeometry`.
12. **Aperçus live** ajoutés : OFFSET, GEAR, ELLIPSE, SPLINE.

Session 3 (3D interactif) :
13. **Ouvrir DXF/DWG/STEP directement** via File>Open (`loadFile` aiguille par
    extension) + verbe IPC `open` unifié.
14. **STEP → vue 3D directe** (`documentIsSolidsOnly` → bascule auto) ; bouton
    3D mémorisé ; verbe IPC `view3d` ; capture 3D via `V3d_View::Dump`.
15. **Sélection 3D interactive** : highlight au survol (AIS MoveTo, modes
    solid/face/edge) + clic = sélection (signal `picked`).
16. **Assemblage** : champ `component` sur SolidEntity ; import STEP additif
    (`Insert STEP as component` + IPC `insertstep`) ; panneau Assembly (arbre) ;
    commandes MOVE3D / ROTATE3D (`SolidEntity::applyTrsf`). Leçon : un `return`
    manquant dans insertStepFile → SIGILL dans le pilote GL ; double refresh 3D
    aussi à éviter.

Session 4-6 (3D interactif approfondi) :
17. Pick 3D HiDPI (devicePos), surbrillance orange (LocalSelected), couleur+
    transparence des solides (TRANS), autocomplétion (QCompleter), verbe IPC
    `pick3d`/`view3d`/`insertstep`.
18. Menu clic droit panneau Assembly (couleur/transparence/renommer/supprimer).
19. Push/Pull d'une face (clic droit 3D → prisme+booléen ; `solidops::pushPullFace`).
20. Ctrl+Z/Ctrl+Y.
21. **Plan de travail généralisé** (`WorkPlane` = origine+normale+axeX ;
    `documentWorkplane(doc)`) + **Sketch sur face** (clic droit → `planeFromFace`
    → plan de travail + bascule 2D ; EXTRUDE perpendiculaire à la face).

## Backlog encore ouvert (suite) — vers l'ergonomie Fusion
- **Sketch sur face v2** : environnement de sketch réorienté (vue face à la
  face) + silhouette de la face projetée en référence (on dessine encore dans
  le canevas 2D abstrait).
- Contrainte d'assemblage (mate/align), gizmo de positionnement 3D interactif,
  MOVE3D en pointant un point 3D, historique de features (paramétrique).
- Aperçus FILLET/CHAMFER/TRIM/EXTEND (survol/sélection).
- MTEXT word-wrap par largeur de colonne ; dimpost (suffixe DIMSTYLE code 3).
- Snap nearest/tangent/node ; tolérance de snap configurable.
- **#39 à re-confirmer avec Lex** : la capture « objets mal orientés » montrait
  deux hexagones à ellipses — le fichier exact n'est pas confirmé (les ellipses
  de Bichonnerie sont en model-space, pas dans des blocs). Les deux correctifs
  ellipse couvrent le miroir/affine ET l'extrusion Z<0 ; à valider visuellement
  quand Lex indique le fichier/zone précis.

## Comment travailler (opérationnel)

- Build : `cd /home/lex/computer/vikicad && ./scripts/build-and-test.sh`
  (build/debug). ASan : build/asan (cmake -DCMAKE_CXX_FLAGS=-fsanitize=...).
- **Lancer la GUI : TOUJOURS `systemd-run --user --unit=vikicad-gui --collect
  /home/lex/computer/vikicad/build/debug/gui/vikicad`** (jamais nohup — le
  sandbox fauche les process ; leçon documentée). Redémarrer : systemctl --user
  stop vikicad-gui d'abord (+ reset-failed si failed).
- Piloter la GUI : `./build/debug/cli/vikicad-cli connect open|exec|query|screenshot`.
- Capture d'écran du bureau : `spectacle -b -n -o out.png`. xdotool NON installé.
- Debug import : env `VIKI_IMPORT_DEBUG=1` (trace addBlock/endBlock/addInsert/
  addLine sur stderr + premier setError du lecteur DXF).
- DWG : R14-2013 natif (libdwgr) ; 2018+ (AC1032) via fallback `~/.local/bin/dwg2dxf`
  (LibreDWG 0.13.3 compilé maison, sources dans scratchpad). Détection auto.
- Fichiers de test importés dans /home/lex/computer : pyramide.vkd,
  bichonnerie.vkd, ligne-de-temps.vkd, base_black.vkd (STEP).
  Non encore testés depuis la clé : planifplusSV.dwg, Ligne de temps.dwg,
  Pliage_pour_Cartes_MTG.dwg.
- Réseau : Ethernet IPv6-only (GitHub inaccessible ; pools Ubuntu/GNU OK).
  sudo indisponible (mot de passe interactif) — demander à Lex si besoin.

## Architecture — rappels critiques (ne pas violer)

- Tout mm en interne ; conversions aux frontières seulement.
- Commandes = machines à états dans core/cmd/Commands*.cpp ; TOUS les paramètres
  numériques AVANT la sélection d'entités (le glouton EntitySet avale les nombres).
- Undo = journal du Document ; toute mutation via addEntity/removeEntity/
  beginModify+endModify dans une transaction.
- Un clic à un prompt Keyword = Finish (accepter défaut). Espace = Enter sauf
  prompt Text. Enter à vide = répéter dernière commande. Saisie distance directe
  (nombre nu à un prompt Point = distance vers le curseur via ctx.pointerHint).
- Angles DXF : INSERT radians / TEXT degrés / MTEXT vecteur code 11 sinon
  heuristique (LESSONS.md). Bulge+ = CCW = à droite du parcours.
- Patches vendored libdxfrw : 0001 fit points spline, 0002 XDATA entités,
  0003 MTEXT Embedded Object (code 101), 0004 retours de ligne bruts dans les
  valeurs. Dossier third_party/libdxfrw/patches/.
- GUI : QT_QPA_PLATFORM=xcb forcé (Wayland crash + Xw_Window). Vue 3D créée
  paresseusement au premier toggle 3D.

## Backlog v2 (après les retours Lex)

Icônes vectorielles ; build Release + bascule du lanceur (.desktop pointe sur
build/debug !) ; cercle TTR, arcs tangents ; pick-point hatch ; MLEADER ;
plans de travail sur face + picking d'arêtes 3D (AIS) ; éditeur MText in-canvas ;
dialogue DimStyle ; word-wrap MText ; XDATA VIKI_ARRAY à l'export DXF ;
« 0 entité importée » doit émettre un warning ; hauteurs de texte annotatives.
