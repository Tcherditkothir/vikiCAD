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

- [x] **G1 — Import + rendu fidèle** — **FAIT (clôturé le 2026-07-16, voir
  DEVLOG)** : parseur RS-274X (FS/MO/AD/ADD/AM, D01/02/03,
  G01/02/03/36/37/74/75, LPC/LPD, attributs X2 nus + forme commentaire
  Altium) ; parseur Excellon (TZ/LZ, PLATED/NON_PLATED, slots) ; « ouvrir
  un kit » = répertoire OU fichier seul (CLI + GUI + IPC), calques nommés/
  colorés par fonction, élection de contour tolérante (GKO>GM1>GM13 +
  plausibilité trait/étendue), keepout peint sous le cuivre, kit entier =
  UNE transaction ; rendu LPC par pixmap ARGB par calque. Validé sur les
  2 kits Altium réels (décodage manuel ligne à ligne + .DRR/.REP comme
  vérités indépendantes) + goldens synthétiques + gui-smoke (149 checks).

  **Dette G1 assumée (à réévaluer en G2/G3)** :
  - `%SR` non-identité = erreur explicite (aucun kit réel ne l'utilise) ;
  - G85 (slots au format Gerber) absent ; exposure 0 dans les macros = refus ;
  - tracé à aperture RECTANGLE (D01 avec ADD..R) rendu en trait à bouts
    ronds — exact seulement pour les apertures C ; à faire exact en G3
    (l'export doit régénérer la vraie empreinte) ;
  - ~~`gerber-ref-diff.sh` écrit mais jamais exécuté en réel~~ → **FAIT
    (2026-07-17)** : gerbv installé, 32/32 couches VERTES, seuils calibrés
    sur les chiffres réels (dhash ≤ 170 — max observé 132 sur le cuivre
    PCBB.GBL, pur halo d'anti-aliasing vérifié à l'œil —, ink-delta ≤
    3 pts). Le script tourne aussi en stage final de gui-smoke (~12 s,
    SKIP silencieux si gerbv/kits absents) ;
  - ~~perçages rendus en ANNEAUX vs disques pleins chez gerbv~~ → **FAIT
    (2026-07-17)** : les hits Excellon (tag `plated` présent) se rendent
    en disques PLEINS (dhash .TXT : 58/104 → 11/40) ;
  - ~~l'élection de contour est une heuristique sans échappatoire~~ →
    **échappatoire livrée (2026-07-17, G2)** : rôle Gerber réassignable
    (`LAYER <calque> ROLE Outline`, ou clic droit dans le LayerPanel →
    « Set Gerber role ») — recolorie à la palette du rôle et déplace au
    rang de peinture du rôle. L'heuristique d'élection reste inchangée ;
  - ~~couche « 0 » vide toujours présente après import kit~~ → **FAIT
    (2026-07-17, G2)** : un kit importé dans un document VIERGE supprime la
    couche « 0 » résiduelle (le calque courant devient la première couche du
    kit) ; le `.vkd` sauvé sans ligne « 0 » ne la ressuscite pas au
    chargement. Les flux DXF/2D ne changent pas (toute entité ou référence
    de bloc sur « 0 » la conserve).
- [x] **G2 — Ergonomie CAM** — **FAIT (clôturé le 2026-07-17, revue
  adversariale passée, voir DEVLOG)** : gestion de pile (presets de
  couleurs/visibilité, transparence), mesures et cotes SUR les gerbers,
  inspecteur d'apertures, rapports (apertures, perçages par diamètre).

  **Fait (2026-07-17) — la pile de couches façon CAM** :
  - transparence par calque (`Layer.alpha` 0-100, défaut opaque), persistée
    .vkd + JSON, éditable LayerPanel (colonne Alpha) et `LAYER <n> ALPHA x` ;
    rendu : le calque translucide passe par le même composite ARGB que les
    calques LPC, opacité appliquée au blit (le calque fond d'un bloc, les
    recouvrements internes ne s'assombrissent pas) ;
  - ordre de dessin générique par rang de calque (`Layer.rank`, plus petit
    = peint d'abord ; égalité = ordre du document, donc les documents
    pré-G2 rendent à l'identique), stable-sort au rendu ; l'importeur de
    kit pose désormais ses rangs SUR les calques (la mécanique
    perçage/contour-au-dessus est devenue générique) ; `LAYER <n> UP|DOWN|
    RANK x` + boutons ▲▼ du LayerPanel ;
  - rôle Gerber réassignable (`Layer.gerberRole` : Copper-Top/Copper-Bottom/
    Mask/Silk/Paste/Outline/Drill/Mech/None), posé par l'importeur, édité
    par `LAYER <n> ROLE r` et le menu contextuel du LayerPanel — assigner
    un rôle recolore à la palette et re-range au rang du rôle ;
  - presets `BOARDVIEW TOP|BOTTOM|ALL` (+ menu View > Board view) : TOP =
    côté bottom atténué à 25 %, BOTTOM = côté top atténué ET **vue miroir X
    au niveau caméra** (vraie vue côté soudure : picking/snaps intacts, la
    sérigraphie bottom se lit à l'endroit), ALL = tout opaque, miroir off,
    rendu identique à l'initial (vérifié à l'octet près en gui-smoke).
  - garde-fous : suite ctest (test_layer_stack + test_gerberkit étendu),
    gui-smoke bloc `stack:` sur S5M0PCBA (16 checks), gerber-ref-diff
    toujours 32/32 (les rendus par défaut n'ont pas bougé).

  **Fait (2026-07-17) — mesurer sur les gerbers comme un outil CAM** :
  - snaps CAM : le point d'insertion d'un Insert (= origine de flash d'une
    pastille GBR-*) est offert en Endpoint ET en **Center** — coter de
    centre de pastille à centre de pastille marche avec le seul osnap
    Center ; extrémités/milieux des traces larges et centres d'arcs/perçages
    déjà couverts par les snapPoints existants (verrouillé par test_snap) ;
  - **MINDIST <idA> <idB>** (alias MD, sélection préalable honorée) : LA
    mesure de clearance — distance minimale BORD À BORD avec sémantique
    matière (trace large = empreinte à bouts ronds, largeur/2 de chaque
    côté ; cercle/perçage = rayon ; pastille = l'empreinte RÉELLE du bloc
    GBR-* à travers la transformation d'insert ; région/pour = anneaux
    remplis ; texte et cie = repli bbox DIT honnêtement : method
    `exact`→`bbox` + note). Noyau : soupe capsules/disques/polygones dans
    core/edit/MinDist.cpp, réutilise l'aplatissement du renderer ;
    recouvrement/contact → distance 0 + `overlap:true` (y compris
    containment pur : perçage entièrement DANS sa pastille). Sortie : ligne
    humaine, points les plus proches, et un trailer JSON compact parsable
    (`mindist.mm/pa/pb/method/overlap`) ; ligne témoin pointillée + tics
    sur le canvas jusqu'à la prochaine commande (overlay transitoire du
    CommandContext, jamais dans les captures clean). Tests aux valeurs
    calculées à la main (traces parallèles 1.6, trace-cercle 3.8, pastilles
    3.0, perçages réels 7.510364...) ;
  - cotes sur kit réel : DIMALIGNED centre-pastille → centre-pastille
    vérifié sur S5M0PCBA (unitaire + gui-smoke bloc `measure:` : MINDIST
    perçage-perçage recoupé avec la formule à la main, cote posée, capture
    clean stable, UNDO restaure le rendu à l'identique).

  **Fait (2026-07-17) — l'inspection : comprendre CE QU'EST chaque objet** :
  - fondation pensée G3-export : l'importeur Gerber tagge chaque entité
    peinte par une aperture avec `dcode:N` (les régions G36/G37 n'en ont
    pas — pas d'aperture) et persiste la TABLE D'APERTURES du fichier dans
    le `camMeta` du calque (.vkd + `cam` dans `query layers`) : forme
    (Circle/Rect/Obround/Polygon/Macro), params en mm, trou, `desc`
    lisible, compte d'usages ; les macros Altium exploitent le commentaire
    `G04:AMPARAMS|...` (la vérité niveau-designer : « RoundedRect
    0.600x0.900 r=0.054 rot 270deg ») ; l'importeur Excellon tagge
    `tool:"Tn"` et persiste la table d'outils (dia mm, plated, usages) ;
  - inspecteur PropertiesPanel : une entité gerber sélectionnée raconte son
    histoire en rangées lecture seule (D-code, aperture, polarité
    dark/clear, outil + platage des perçages, rôle du calque) ; nouvelle
    commande générique `SELECT [ids]` (alias SEL, vide = déselection) pour
    piloter le pickfirst headless, et `query ui` expose les rangées du
    panneau (`propRows`) — l'inspecteur est vérifiable par agent ;
  - `APERTURES [calque]` (alias APER, sans argument = tous les calques à
    table) : table alignée D-code / desc / usages + trailer JSON ; test
    kits réels : l'ensemble {usage>0} == la liste « Used DCodes » du .REP,
    couche par couche, sur LES DEUX kits (l'égalité stricte inclut le GKO
    keepout sans apertures de PCBB) ;
  - `DRILLREPORT` (alias DR) : table de perçage par diamètre+platage sur
    les cercles VIVANTS du document (un trou effacé disparaît du rapport),
    trailer JSON ; test golden : chaque outil du .DRR retrouvé (platage,
    diamètre ±0.01, compte exact) et totaux égaux (182 PCBA / 330 PCBB) ;
  - garde-fous : test_cam_inspect (8 cas dont 2 golden kits), gui-smoke
    bloc `inspect:` (APERTURES verbatim AMPARAMS, DRILLREPORT == .DRR,
    couche « 0 » absente, panneau via SELECT + query ui) — 200 checks
    verts, refdiff toujours 32/32.

  **Clôture (2026-07-17) — revue adversariale numérique passée** : DRILLREPORT
  == .DRR et APERTURES == .REP sur les deux kits, MINDIST recoupé à la main
  à 1e-9 depuis les fichiers bruts (rotations d'insert comprises),
  alpha/rank/role survivent save-as + reopen, .vkd pré-G2 s'ouvrent avec les
  défauts. Deux défauts MinDist trouvés : le **majeur corrigé** (parité
  even-odd inter-anneaux → un objet 100 % DANS une pastille macro
  multi-anneaux, ex. RoundedRect Altium = 2 rects + 4 disques qui se
  RECOUVRENT, sortait « 0.196 mm exact, overlap:false » ; la matière est
  maintenant l'UNION des anneaux, comme au rendu — test unitaire + repro
  kit réel verts), le mineur acté en dette ci-dessous. Overlay MINDIST
  rendu lisible à l'échelle carte (halo sombre sous les pointillés).

  **Dette G2 assumée (à réévaluer en G3)** :
  - les flashes RONDS sont cuits en polygones INSCRITS dans le cercle
    (sagitta ~1e-3 mm, ex. 32-gone pour la pastille D67 de 0.4064 mm) →
    MINDIST sur-estime la clearance pastille-pastille jusqu'à ~0.002 mm
    dans les directions obliques (D67-D67 réel : 5.184919 rendu vs
    5.183816 vrai). Non-conservateur pour du contrôle de clearance, et
    au-delà de la précision promise par le commentaire kMeasureTol de
    MinDist.cpp. À corriger en G3 (l'export doit régénérer les empreintes
    exactes de toute façon : disques analytiques ou compensation
    mid-ordinate) ;
  - le snap Center sur le point d'insertion vaut pour TOUT insert, pas
    seulement les blocs GBR-* (assumé — voir LESSONS 2026-07-17) ;
  - MINDIST reste O(nA×nB) par paire de primitives — suffisant pour deux
    entités, à revisiter si un jour on mesure calque contre calque.
- **G3 — Édition + export** : édition avec tout l'outillage 2D existant,
  export RS-274X + Excellon (round-trip golden : import→export→réimport =
  géométrie identique), panélisation (réseaux existants → SR ou dépliage),
  pont DXF↔Gerber (contour de carte, trous de fixation — l'interface
  méca-élec, LA force différenciante de VikiCAD).

  **Fait (2026-07-17) — écrivain RS-274X (`core/io/GerberWriter.{h,cpp}`)** :
  - API `exportGerberLayer(doc, layerName, path)` / `writeGerberLayer(...,
    QByteArray&)` + warnings collectés ; dialecte moderne écrit :
    `%FSLAX46Y46*%` (absolu, 1e-6 mm), `%MOMM*%`, G75, G01/G02/G03,
    D01/D02/D03, `%LPD*%`/`%LPC*%` émis en suivant l'ORDRE de peinture du
    document (tag `gpol` des entités), G36/G37 pour les Hatch solides,
    en-tête G04 + `TF.GenerationSoftware`, M02 ;
  - table d'apertures RÉGÉNÉRÉE depuis les entités : trace au width intact →
    la définition D'ORIGINE de `Layer.camMeta` revient telle quelle (un tracé
    à aperture RECT ressort en R — gerbv peint alors l'exporté comme
    l'original, la dette G1 « bouts ronds » est contournée à l'export) ;
    width édité → nouvelle entrée `C,<w>` ; pastille : macro %AM re-émise
    VERBATIM (nouvelle table `camMeta.macros`, primitives en mm, posée par
    l'importeur et persistée .vkd) ; échelle uniforme/rotation d'insert
    repliées dans les paramètres standard (C : toute rotation ; R/O :
    multiples de 90° ; P : rotation quelconque ; trou scalé) ; transform
    non-uniforme, rect hors-axe, macro scalée ou bloc sans camMeta →
    macro outline (primitive 4) reconstruite de la géométrie du bloc +
    warning ; dédup (forme, params arrondis 1e-6), numérotation D10+ ;
  - garde-fous : `test_gerber_export.cpp` — round-trip des 12 goldens
    synthétiques à 1e-6 mm, cas de régénération (dédup, width édité, insert
    scalé/tourné, fallback outline, .vkd save/load des macros), et LE TEST
    DE VÉRITÉ : les 28 couches Gerber non vides des 2 kits réels, gerbv
    rend l'ORIGINAL et l'EXPORTÉ (notre renderer hors boucle) — observé
    dhash ≤ 1/1024 et delta d'encre ≤ 0.002 pt (seuils : <30 / ≤1 pt),
    plus ré-import sémantique (mêmes entités, coordonnées à 1e-3).
    SKIP sans kits ou sans gerbv.

  **Dette G3-export assumée** :
  - la dette G2 « flashes ronds cuits en polygones inscrits (~2 µm) » reste :
    l'export re-génère les empreintes standard depuis camMeta (analytique,
    pas depuis les anneaux cuits), seul le fallback outline hérite de la
    tessellation inscrite — MINDIST garde son biais documenté ;
  - pas encore de commande utilisateur (CLI/GUI) pour l'export : API C++
    seulement, l'intégration commande + Excellon writer + panélisation
    restent à faire dans la suite de G3 ;
  - entités sans image Gerber (texte, cotes, hatch non solide) → warning +
    skip ; cercles Excellon (`plated`) exportés en région disque si on
    exporte une couche de perçage en Gerber (l'écrivain Excellon est le
    vrai chemin, warning émis).

## Stratégie de test (process obligatoire habituel)

- Goldens synthétiques petits commités dans `tests/golden/gerber/` (chaque
  construct du format isolé : flash rect, arc MQ, région, LPC, SR…).
- Kits réels de Lex dans `/home/lex/computer/pcb-ref/` (HORS repo — ses
  cartes restent privées) ; tests golden optionnels, SKIP si absent.
- Renderer de référence : `gerbv` (paquet Ubuntu, export PNG en CLI) →
  `scripts/gerber-ref-diff.sh`, exécuté aussi en stage final optionnel
  de gui-smoke (SKIP silencieux quand gerbv ou les kits manquent).
- Round-trip : import→export→réimport, comparaison sémantique (comme DXF).

## Prérequis en attente

- [x] Kits Gerber réels fournis par Lex → `/home/lex/computer/pcb-ref/`
      (S5M0PCBA + S5M0PCBB, Altium Designer 18.0.9)
- [x] `sudo apt install gerbv` → installé (2026-07-17), ref-diff calibré
