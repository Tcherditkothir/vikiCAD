# REPRISE — état de travail au 2026-07-08 (fin de session M6-usage)

Document de reprise pour continuer les corrections. À lire AVANT toute action,
avec DEVLOG.md (historique) et LESSONS.md (pièges connus).

## État global

- Plan M0→M8 livré, tags git `m0`…`m8`. **85/85 tests verts** (`scripts/build-and-test.sh`).
- Phase actuelle : **M6-usage** — Lex utilise VikiCAD sur ses vrais fichiers
  (clé USB TRANSCEND, `/dev/sda1`, monter via `udisksctl mount -b /dev/sda1`)
  et remonte les bugs un par un. C'est LA priorité : réactivité sur ses retours.
- Dernier commit : « patch 0004 tomato bug ». Tout est commité au fil de l'eau.

## Chantiers OUVERTS (retours Lex non résolus)

1. **« Encore un peu d'erreurs sur les blocs de texte »** (MTEXT) — pas encore
   diagnostiqué. Pistes déjà traitées (ne pas refaire) : hauteur écrasée par la
   section Embedded Object (patch 0003), rotation par vecteur code 11, heuristique
   radians/degrés code 50. Pistes restantes probables : alignement/attachment
   MTEXT (code 71, 1=TopLeft… mon TextEntity ancre en baseline-left, AutoCAD
   ancre selon 71 → décalages), largeur de colonne (word-wrap non implémenté),
   interligne (code 44), formats inline `{\\f...;}` `\\H` non nettoyés (cleanMtext
   ne traite que \\P). DEMANDER à Lex un exemple précis (quel texte, quel écart).
2. **« Beaucoup de travail sur les snaps »** — pas de détail. Points faibles
   connus de snapQuery (core/snap/SnapEngine.cpp) : perpendiculaire seulement
   depuis lastPoint ; pas de snap tangent/nearest/node ; l'intersection n'est
   calculée qu'entre candidats dont la BBOX contient le curseur (rate les
   intersections dont un des deux objets a une grosse bbox ? non — contient le
   curseur = ok, mais VÉRIFIER) ; tolérance fixe 10 px ; pas d'aimantation
   visuelle du curseur pendant le drag de grips ; pas de snap DANS les inserts
   (les sous-entités de blocs n'émettent pas leurs snapPoints via InsertEntity !
   → InsertEntity::snapPoints ne donne QUE le point d'insertion — gros manque
   probable). Interroger Lex sur les cas exacts, mais le snap-dans-les-blocs
   est quasi certainement le gros morceau.

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
