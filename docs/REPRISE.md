# REPRISE — état de travail au 2026-07-08 (fin de session M6-usage)

Document de reprise pour continuer les corrections. À lire AVANT toute action,
avec DEVLOG.md (historique) et LESSONS.md (pièges connus).

## État global

- Plan M0→M8 livré, tags git `m0`…`m8`. **85/85 tests verts** (`scripts/build-and-test.sh`).
- Phase actuelle : **M6-usage** — Lex utilise VikiCAD sur ses vrais fichiers
  (clé USB TRANSCEND, `/dev/sda1`, monter via `udisksctl mount -b /dev/sda1`)
  et remonte les bugs un par un. C'est LA priorité : réactivité sur ses retours.
- Dernier commit : « patch 0004 tomato bug ». Tout est commité au fil de l'eau.

## Chantiers récents — TRAITÉS le 2026-07-08, en attente de validation Lex

1. **Blocs de texte (MTEXT/TEXT)** — corrigé (commit 82d63a7) : attachment
   point code 71 + vAlign/lineSpacing sur TextEntity, justification TEXT
   codes 72/73/11, décodage complet des codes inline ({\f...;}, \H, \S
   fractions, \U+XXXX, %%d/%%c/%%p), export symétrique. Reste au backlog :
   word-wrap par largeur de colonne, dimpost (suffixe DIMSTYLE code 3).
2. **Snaps** — le gros manque (aucun snap dans les blocs) est corrigé :
   snapQuery récurse dans les définitions (points transformés, imbrication
   prof. 4). Restent au backlog si Lex en redemande : snap nearest/tangent/
   node, perpendiculaire depuis autre chose que lastPoint, aimantation
   pendant le drag de grips, tolérance configurable.
3. **Cotes** (découvert en validant) : DIMSTYLE importé (tailles ×DIMSCALE),
   override code 1 avec substitution `<>`, styleScale sur Dimension/Leader
   suivi par les transformations (cotes dans blocs échellés). Les vignettes
   McMaster de Bichonnerie sont maintenant propres.
4. **ZOOM W** ajouté (fenêtre par deux coins) — pratique pour les captures
   IPC : `vikicad-cli connect exec "ZOOM W x1,y1 x2,y2"`.

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
