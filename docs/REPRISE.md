# REPRISE — état de travail au 2026-07-08 (fin de session M6-usage)

Document de reprise pour continuer les corrections. À lire AVANT toute action,
avec DEVLOG.md (historique) et LESSONS.md (pièges connus).

## État global

- Plan M0→M8 livré, tags git `m0`…`m8`. **99 cas de tests verts / 786 assertions**.
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

## Backlog encore ouvert (suite)
- 3D : contrainte d'assemblage (mate/align), gizmo de positionnement 3D,
  highlight persistant de la sélection, MOVE3D interactif (pointer un point 3D).
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
