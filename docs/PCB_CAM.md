# Chantier PCB CAM — Gerber/Excellon (décidé avec Lex le 2026-07-16)

## Décision de périmètre (brainstorm du 2026-07-16)

- **OUI : éditeur CAM de fichiers de fabrication.** Lire, inspecter, mesurer,
  coter, éditer et réexporter Gerber RS-274X (+ attributs X2) et Excellon.
  Trou réel dans l'écosystème : gerbv et GerbView ne font que visualiser ;
  les éditeurs Gerber sont des outils Windows à plusieurs milliers de dollars.
- **NON : EDA complet.** Pas de nets, pas de routage, pas de DRC, pas de
  footprints. C'est le territoire de KiCad (25 ans de travail). VikiCAD fait
  de la *chirurgie de fichiers de fab*, pas de la conception PCB. Ce contrat
  doit rester visible pour l'utilisateur (éditer du cuivre sans nets peut
  casser une carte — comme dans tout outil CAM).
- **PLUS TARD : schémas** (chantier suivant, scope pré-réfléchi : symboles =
  blocs .vkd + attributs, commande WIRE avec jonctions, auto-incrément des
  références R1/R2/R3, export netlist simple — du dessin intelligent, pas un
  EDA). Lecture .kicad_sch éventuelle en consultation ; round-trip = piège.

## Pourquoi c'est un ajustement naturel

| Gerber | VikiCAD existant |
|---|---|
| Tracé D01, aperture ronde/rect | Polyligne avec épaisseur |
| Arcs G02/G03 (multi-quadrant G75) | Segments à bulge |
| Flash D03 | Insert de bloc (aperture = BlockDefinition) |
| Régions G36/G37 | Hatch solide |
| Couches cuivre/masque/silk/outline | Calques |
| Excellon (perçages, T-codes) | Cercles/points sur calque |
| Attributs X2 (netname, .FileFunction) | JSON inconnu préservé par entité |
| mm/inch (%MO*, INCH/METRIC) | mm canonique, conversion aux frontières |

## Les 3 défis techniques réels

1. **Polarité LPC/LPD** (dessin négatif qui efface le dessin précédent).
   Rendu : modes de composition QPainter dans le pipeline pixmap statique.
   Édition : la sémantique est un ORDRE DE PEINTURE — préserver l'ordre des
   entités du calque (champ `sort` existant). Le concept vraiment nouveau.
2. **Réécriture RS-274X** : regénérer la table d'apertures à l'export (piste
   éditée 0.2→0.25 mm = nouveau D-code ; dédupliquer, numéroter D10+).
   Excellon : zéro-suppression et modes d'unités (dialectes) à gérer.
3. **Performance** : une carte dense = dizaines de milliers de tracés, au-delà
   de la cible d'origine (« quelques milliers »). Atouts : R-tree, blocs
   instanciés pour les flashes, LOD. Mesurer AVANT d'optimiser (fichier
   torture réel de Lex).

## Phases

- **G1 — Import + rendu fidèle** : parseur RS-274X (FS/MO/AD/ADD, D01/02/03,
  G01/02/03/36/37/75, LP, SR step-repeat, attributs X2 TF/TA/TO préservés) ;
  parseur Excellon ; « ouvrir un kit » = pile de fichiers → calques nommés
  par fonction ; rendu incluant polarité. Validation : diff visuel contre
  gerbv (export PNG CLI) sur les kits réels de Lex + goldens synthétiques
  commités.
- **G2 — Ergonomie CAM** : gestion de pile (board multi-fichiers, presets de
  couleurs/visibilité, transparence), mesures et cotes SUR les gerbers,
  inspecteur d'apertures, rapport (compte de perçages par diamètre…).
- **G3 — Édition + export** : édition avec tout l'outillage 2D existant,
  export RS-274X + Excellon (round-trip golden : import→export→réimport =
  géométrie identique), panélisation (réseaux existants → SR ou dépliage),
  pont DXF↔Gerber (contour de carte, trous de fixation — l'interface
  méca-élec, LA force différenciante de VikiCAD).

## Stratégie de test (process obligatoire habituel)

- Goldens synthétiques petits commités dans `tests/golden/gerber/` (chaque
  construct du format isolé : flash rect, arc MQ, région, LPC, SR…).
- Kits réels de Lex dans `/home/lex/computer/pcb-ref/` (HORS repo — ses
  cartes restent privées) ; tests golden optionnels, SKIP si absent.
- Renderer de référence : `gerbv` (paquet Ubuntu, export PNG en CLI) →
  diff d'images comme dans gui-smoke. À installer (sudo requis → Lex).
- Round-trip : import→export→réimport, comparaison sémantique (comme DXF).

## Prérequis en attente

- [ ] Kits Gerber réels fournis par Lex → `/home/lex/computer/pcb-ref/`
- [ ] `sudo apt install gerbv` (2.10.0 dispo au pool Ubuntu — IPv6 OK)
