# VikiCAD — Erreurs et leçons apprises

Log continu des erreurs commises, impasses, et leçons techniques. Ajouter au fil de l'eau, ne jamais réécrire l'historique.

---

## 2026-07-07

- **Leçon (recherche préalable) :** OCCT n'a AUCUNE voie supportée pour exporter des notes texte AP242 (`XCAFDoc_NotesTool` ne traduit pas vers STEP). Décidé d'avance : sidecar JSON d'abord, injection d'UDA AP242 time-boxée plus tard. Ne pas redécouvrir ça en M7.
- **Leçon (environnement) :** toujours vérifier réseau + sudo AVANT de planifier une session d'installation ; la machine peut être hors-ligne. Le squelette a pu être écrit intégralement hors-ligne grâce au guard `if(EXISTS)` sur libdxfrw — pattern à retenir pour tout vendoring.
- **À vérifier à la première compilation :** noms des toolkits OCCT 7.9 sur Ubuntu (TKSTEP → TKDESTEP depuis 7.8 pour l'échange de données — le lien actuel de vikicore n'utilise que les toolkits de modélisation, à étendre en M7). → Vérifié : les toolkits de modélisation portent les noms attendus.
- **Piège Qt :** ne jamais nommer une fonction `emit` — c'est une macro Qt (keywords). Renommée `emitJson` dans le CLI.
- **Leçon (conception script)** : la première version du ScriptRunner soumettait chaque ligne en mode strict, ce qui cassait les commandes multi-lignes façon .scr d'AutoCAD (`LINE` sur une ligne, points sur les suivantes). Corrigé avant même la compilation en relisant le comportement attendu. Les sémantiques d'AutoCAD sont la référence de moindre surprise pour ce projet.
- **Leçon (undo)** : le choix « journal enregistré par le Document » a tenu ses promesses dès M1 — ERASE/UNDO/REDO ont fonctionné sans écrire une ligne d'undo dans les commandes, et le filet « rollback si transaction ouverte à l'annulation » a un test dédié.
- **Leçon (grammaire de commandes)** : le glouton EntitySet (qui avale tous les tokens numériques) a mordu TROIS fois (ARRAYRECT, test store, FILLET3D). Règle désormais gravée : **dans toute commande headless, les paramètres numériques viennent AVANT la sélection d'entités.**
- **Leçon (sanitizers)** : ASan a attrapé un use-after-free réel (TRIM/BREAK) qui passait tous les tests fonctionnels. La passe sanitizers avant chaque tag n'est pas du luxe.
- **Leçon (bulge DXF)** : bulge positif = arc CCW = à DROITE du sens de parcours pour une corde +x. Mon intuition disait l'inverse ; la dérivation (3 méthodes concordantes : spec, Lee Mac, QCAD) a tranché contre le test, pas contre le code.
- **Leçon (libdxfrw)** : trois lacunes réelles trouvées et traitées — fit points de spline jamais écrits (patch 0001), XDATA d'entité jamais écrits (patch 0002), boucles polyligne de hatch « writeme » (contourné par boucles d'arêtes). Le choix « vendored patchable » du plan était le bon.
- **Leçon (OCCT)** : STEPControl écrit ses statistiques sur stdout — mortel pour un CLI JSON ; museler `Message::DefaultMessenger` d'entrée. Et le Plan A AP242 (attributs utilisateur) est passé du premier coup une fois le contexte de représentation non-null — plus facile que craint, le sidecar reste quand même.
- **Leçon (méta)** : le plan initial complet (M0→M8) a tenu en une session parce que chaque jalon finissait testé-vert et taggé avant d'ouvrir le suivant, et parce que le CLI-harnais de test existait dès M1 — les deux meilleures décisions du plan.
- **Leçon (fausse piste)** : « le logiciel se ferme tout seul » n'était PAS un bug de VikiCAD — c'étaient les instances GUI lancées par l'agent depuis ses commandes sandboxées, fauchées à la fermeture du sandbox. Diagnostic par élimination : lancement via `systemd-run --user` → stable indéfiniment. Règle : l'agent lance la GUI en unité systemd (`vikicad-gui`) ; les lancements utilisateur (menu/icône) n'ont jamais été affectés.
- **Leçon (conventions d'angles DXF — le « massacre » du DWG)** : libdxfrw N'EST PAS cohérent entre entités : INSERT code 50 → converti en RADIANS au parse ; TEXT code 50 → laissé en DEGRÉS ; MTEXT code 50 → brut (la spec DXF dit radians). Mon importeur traitait tout en degrés → toutes les rotations de blocs divisées par ~57 → dessin « croche ». S'ajoute : xscale≠yscale et échelles négatives (blocs miroirés) écrasées par mon échelle uniforme. Règle : à CHAQUE champ libdxfrw, vérifier le commentaire du header ET le parseCode. Round-trip testé rotation+miroir+échelle.

## 2026-07-08

- **Leçon (MTEXT code 71)** : dans libdxfrw, l'attachment point du MTEXT atterrit dans `textgen` (hérité de DRW_Text via parseCode 71), PAS dans `alignV` malgré le commentaire du constructeur. Les deux chemins (DXF parseCode et DWG parseDwg) sont cohérents là-dessus. Toujours vérifier le parseCode, pas les commentaires.
- **Leçon (cotes DXF)** : des « textes géants » sur un dessin importé ne sont pas forcément un bug de texte — c'étaient des DIMENSION régénérées avec notre DimStyle par défaut (3,5 mm) dans une zone dessinée à ~0,01 unité. Trois mécanismes distincts à couvrir : la table DIMSTYLE du fichier (tailles absolues ×DIMSCALE), l'override de texte code 1 (avec `<>` = valeur mesurée), et l'échelle d'insertion des blocs (styleScale suivi par transform). Les trois manquaient.
- **Leçon (snap et perfs)** : pour snapper dans les blocs, transformer les POINTS de snap des sous-entités (xf.apply) au lieu de cloner+transformer les entités — même exactitude pour tous les types de points, et aucun coût mémoire par requête souris.
- **Piège (heredoc bash)** : un fixture DXF contient une ligne littérale `EOF` — un heredoc `<< 'EOF'` se fait couper au milieu. Utiliser un délimiteur improbable (`ENDOFTEST`).

- **Leçon (dwg2dxf enveloppe à 254 octets)** : LibreDWG coupe les valeurs de chaîne longues à une largeur FIXE (254 o, parfois 255 quand un caractère UTF-8 de 2 octets chevauche) en insérant un CR/LF brut au milieu de la valeur. Signal robuste pour distinguer une continuation d'un vrai code de groupe : la largeur d'enveloppe. Ne PAS se fier seulement à « ligne non numérique » (une continuation pourrait être purement numérique) ni re-coller inconditionnellement (un DXF natif pourrait avoir une valeur de 254 o légitime). Combiner : segment ≥ 254 ET ligne suivante non numérique. Le DXF bien formé n'a jamais de ligne non numérique là où un code est attendu, donc zéro régression.

## 2026-07-09 — Nuit autonome (workflows)

- **Piège OCCT** : `BRepPrimAPI_Make*::IsDone()` (cylindre, boîte…) renvoie
  souvent `false` tant que la forme n'est pas construite. Ne PAS s'y fier :
  forcer `.Shape()` (ce qui déclenche `Build()`) puis `IsNull()`-checker le
  résultat. A fait échouer le trou paramétrique (« building the hole cylinder
  failed » alors que le cylindre était valide).
- **Leçon (orchestration autonome)** : un agent qui meurt (ex. API 529) en
  laissant l'arbre sale empoisonne TOUS les agents suivants si la règle est
  « abort si arbre sale ». Correctif gravé dans les scripts de nuit : chaque
  agent fait `git reset --hard HEAD` au DÉMARRAGE (le travail commité est sauf
  dans HEAD ; seul le non-commité d'un agent mort saute). + idempotence (grep du
  symbole avant d'implémenter) pour qu'une relance de lot ne duplique rien.
  Séquentiel + reset-au-démarrage + commit-si-vert = arbre jamais cassé.
- **Leçon (résilience 529)** : les surcharges serveur transitoires arrivent en
  fenêtres ; relancer le même lot plus tard passe (les agents retentent en
  interne, mais une fenêtre de surcharge soutenue les tue quand même). Rendre le
  workflow ré-exécutable (idempotent, arbre propre) vaut mieux que d'essayer
  d'éviter le 529.
- **Leçon (récupération)** : quand un agent laisse un travail partiel propre dans
  le stash (ici `makeHole` complet à ~90 %), le finir à la main (enregistrement
  de la commande + test + build) est plus rapide et fiable que de tout jeter et
  relancer — mes propres appels d'outils ne subissent pas la même fenêtre 529.

## 2026-07-10 — Leçons de la passe « professionnel »

- **Piège Qt (grave)** : deux QShortcut (ou QAction+QShortcut) sur la même
  touche → « ambiguous » → AUCUN ne se déclenche, sans erreur ni log. Ctrl+Z
  était mort depuis sa création. Règle : une touche = UN SEUL propriétaire ;
  les raccourcis standards vivent sur les QAction de menu ; tout chargeur de
  raccourcis utilisateur doit filtrer les touches déjà prises.
- **Piège OCCT** : un booléen peut « réussir » (IsDone) en produisant un
  compound SANS solide (ex. prisme d'une face courbe re-fusionné). Toujours
  vérifier TopExp SOLID avant de déclarer ok — sinon l'utilisateur voit sa
  pièce disparaître.
- **Transactions** : Document::undo refuse si une transaction est ouverte —
  correct, mais une fuite (early return/exception entre begin et commit) tue
  l'undo EN SILENCE. RAII (TransactionScope) partout dans la GUI + soupape
  dans UNDO qui rollback et prévient.
- **Process (le plus important)** : reproduire le bug AVANT de corriger
  (headless d'abord), et valider sur la VRAIE GUI via IPC + diff de captures
  (`scripts/gui-smoke.sh`, 40 checks) AVANT de livrer. Les allers-retours
  « corrigé → non ça marche pas » venaient de corrections plausibles jamais
  exécutées dans la GUI réelle.

## 2026-07-16 — Parseur Gerber (G1)

- **Dialecte Altium 18 vérifié sur kits réels** : les flashes du GTL sont des
  `D02` suivis d'un `D03*` NU (flash au point courant, zéro coordonnée) — un
  parseur qui exige des coordonnées sur D03 rate 100 % des flashes. Idem
  `D02*` nu pour fermer les contours de région.
- **Macros Altium** : dans `ROUNDEDRECT` la rotation n'est portée QUE par les
  primitives 21 (rect) ; les cercles de coin (prim 1, 4 paramètres, sans champ
  rotation) ont leurs centres DÉJÀ tournés par Altium. Comparer les macros D15
  (rot 270) et D16 (rot 0) du même fichier l'a prouvé. Appliquer la rotation
  autour de l'origine macro reste correct dans les deux cas.
- **Vérité indépendante** : le `.REP` d'Altium (« Used DCodes » par fichier)
  égale exactement l'ensemble des apertures sélectionnées ET utilisées par des
  opérations — vérifié sur les 30 couches des 2 kits. Excellente vérité de
  test qui ne dépend pas de notre propre parseur.
- **Suppression de zéros** : FSLA (leading omis) => valeur = entier/10^déc ;
  FST (trailing omis) => compléter à DROITE jusqu'à int+déc chiffres d'abord.
  Les deux se confondent sur la plupart des nombres (94488 -> 0.94488 vs
  94.488) — tester les bornes (`X1`, `X1000000`) qui, elles, divergent.
- **Piège Qt (récidive du `emit`)** : `slots` est aussi une macro Qt (keywords)
  — un champ `std::vector<...> slots;` casse la compilation avec un message
  cryptique (« declaration does not declare anything »). Renommé `drillSlots`
  dans ExcellonIo. Liste noire pour les noms : `emit`, `signals`, `slots`,
  `foreach`.
- **Excellon vs Gerber (nommage inversé des zéros)** : Excellon `TZ`/`LZ`
  nomme les zéros CONSERVÉS (TZ = trailing gardés = leading supprimés),
  alors que Gerber %FS L/T nomme les zéros OMIS. Même arithmétique, noms
  croisés — les deux parseurs le documentent côte à côte.

## 2026-07-16 — Ouverture de kit + rendu LPC (G1 suite)

- **LPC = pixmap ARGB par calque, pas un mode de composition global** : un
  CompositionMode_Clear sur le pixmap partagé du canvas efface TOUS les
  calques dessous ; peindre le LPC couleur-fond simule le même bug. Seule
  la composition par calque (LPD peint, LPC troue l'alpha, blit ensuite)
  donne la sémantique Gerber. Les calques sans LPC gardent le chemin rapide.
- **Fausse alerte à connaître (kits 2 couches)** : sur la vue kit complète,
  des « pistes bleues par-dessus le plan rouge » ne sont PAS un bug d'ordre
  de peinture — ce sont les canaux d'isolation LPC du plan top qui laissent
  voir le plan bottom (bleu) en dessous. Le GTL importé SEUL montre les
  mêmes canaux en noir (fond) : c'est le test discriminant.
- **Fixtures Gerber synthétiques** : notre parseur (fidèle au dialecte)
  exige un mode G01 avant le premier D01 — un fixture minimal sans `G01*`
  échoue avec « D01 before any G01/G02/G03 mode ». Toujours copier l'en-tête
  des goldens existants (`%FS` + `%MO` + `G01*`).

## 2026-07-16 — Élection de contour (durcissement G1)

- **Un GKO non vide n'est PAS forcément le contour** : sur S5M0PCBB c'est
  une vraie zone keepout (UN rectangle G36 plein sur l'antenne ESP32).
  L'élire « Outline » (magenta, peint en dernier) = pavé opaque qui masque
  le cuivre. Critère qui discrimine : un contour contient des TRAITS
  (Draw/Arc) ; un fichier fait uniquement de régions pleines est une zone.
  En plus : « Keepout » se peint désormais SOUS le cuivre (rang 5).
- **Le « contour » réel peut n'être qu'un fragment** : le GM1 de PCBB ne
  dessine QUE le bord haut façonné (encoches + languettes — 68 % de la
  largeur, 24 % de la hauteur de la carte). Exiger une couverture sur les
  DEUX axes rejette le vrai contour ; l'heuristique retenue = ≥ 60 % de
  l'étendue de la carte sur AU MOINS un axe.
- **Bbox « grep » trompeuse** : estimer l'étendue d'un Gerber en regexant
  tous les X/Y du fichier compte aussi les D02 (déplacements) — le GM1 de
  PCBB semblait couvrir 88×50 mm alors que ses objets DESSINÉS tiennent
  dans 67×13. Toujours mesurer sur les objets parsés (fileBbox), jamais
  sur le texte brut.
- **Excellon seul via IPC** : `connect open` d'un .TXT de perçage marche
  depuis le fix single-file (sniff M48) — 330 cercles pour PCBB1.TXT,
  utile pour inspecter les perçages sans le reste du kit.

## 2026-07-17 — Premier run réel de gerber-ref-diff (calibration)

- **Fausse alerte (récidive, variante capture)** : le premier run réel
  affichait ~27 % d'écart d'encre sur les 4 couches cuivre et un diagnostic
  « les pistes re-dessinées après LPC sont mangées ». FAUX : la composition
  par calque suit déjà strictement l'ordre du document (vérifié par golden
  synthétique lpc_redraw.gbr + sondes de pixels, ET en injectant exprès le
  bug « deux passes » — les sondes le détectent). Le vrai coupable : les
  DÉCORATIONS du canvas (glyphe UCS, réticule collé aux bords) gonflaient la
  bbox d'encre de la capture côté VikiCAD → crop faussé → dhash/encre
  divergents sur TOUTES les couches. Règle : toute comparaison d'images
  contre un renderer de référence passe par `screenshot PATH clean` (capture
  sans overlays), jamais par le grab décoré.
- **Un masque « plus clair que le fond » perd les calques noirs** : les
  perçages (calque Drill, noir 0x000000) se dessinent PLUS SOMBRES que le
  fond du canvas (24,26,28) — un seuil de luminance les efface du masque.
  Côté VikiCAD le masque d'encre est « différent de la couleur du fond »
  (échantillonnée au coin, zoom-extents garantit la marge), pas un seuil.
- **Divergence résiduelle connue** : perçages = ANNEAUX (CircleEntity
  cosmétique) chez nous vs DISQUES pleins chez gerbv → dhash .TXT plus haut
  (58/104 vs médiane ~25) mais sous seuil. Affichage rempli = candidat G2.
  → RÉGLÉ le jour même : `CircleEntity::buildPrimitives` remplit le disque
  quand l'entité porte le tag `plated` de l'importeur Excellon (dhash .TXT :
  11/40). Le renderer de référence sert aussi à ÇA : transformer une dette
  d'affichage floue en chiffre qui baisse.
- **Une composition en 2 passes ≠ ordre de peinture Gerber** : « tous les
  LPD puis tous les LPC » est SÉduisant (deux boucles simples) mais faux —
  un objet LPD re-dessiné APRÈS un clear doit rester visible dedans, et un
  clear suivant peut le re-manger. La seule sémantique correcte est
  l'alternance SourceOver/Clear objet par objet dans l'ordre du document ;
  verrouillée par le golden lpc_redraw.gbr + 5 sondes de pixels gui-smoke.
- **gerber-ref-diff est maintenant un stage de gui-smoke** (~12 s, SKIP
  silencieux sans gerbv/kits) : la parité visuelle avec gerbv ne peut plus
  régresser sans casser le harnais.

## 2026-07-17 — G2 pile de couches (alpha/rang/rôle, BOARDVIEW)

- **Le comparateur d'encre en niveaux de gris est aveugle au rouge↔bleu** :
  les cuivres top (0xE53935) et bottom (0x3D7EFF) ont des luminances quasi
  égales (108 vs 121, sous le seuil de 16 niveaux du bp gui-smoke) — un swap
  d'ordre de pile peut rendre « 0 pixel changé » en gris alors que le plan a
  visiblement changé de couleur. Valider les inversions de pile par sonde
  RGB (pixel du plan : (229,57,53) → (61,126,255)), pas par le diff gris.
- **Égalité de rang = ordre du document** : le tri stable par rang de calque
  ne change RIEN quand tous les rangs sont à 0 — c'est ce qui garantit que
  les documents pré-G2 (et les hashes gui-smoke/refdiff existants) rendent
  bit-identique. Toute future clé de tri au rendu doit garder cette
  propriété « défaut = comportement historique ».
- **Miroir X au niveau caméra = gratuit partout** : en le posant dans les
  DEUX fonctions de mapping de Camera2d (worldToScreen/screenToWorld) +
  panPixels, picking, snaps, zoomAt et rubber band marchent sans une ligne
  de plus — et la sérigraphie bottom (stockée en miroir dans le Gerber) se
  lit à l'endroit, comme dans un vrai viewer CAM. Seul drawGrid a demandé
  une normalisation min/max de la fenêtre monde.

## 2026-07-17 — G2 mesure (MINDIST, snaps pastilles)

- **La distance « matière » se calcule sur la soupe du renderer** : plutôt
  qu'un algorithme par paire de types (n² de cas spéciaux), chaque entité
  se réduit via son propre buildPrimitives en capsules (stroke width/2),
  disques et polygones remplis — segment-segment/point-segment font le
  reste. Les caps ronds Gerber rendent ça EXACT pour les traces : distance
  des axes moins les demi-largeurs. Seuls les cercles gardent un chemin
  analytique dédié (disque plein, pas d'aplatissement).
- **Un bord-à-bord positif peut mentir : tester le containment.** Un
  perçage entièrement DANS sa pastille a une distance de FRONTIÈRE
  positive alors que la matière se recouvre — après la passe par paires,
  tester un point représentatif de chaque forme contre les polygones de
  l'autre (even-odd), sinon la « clearance » d'un via annulaire sort
  fausse.
- **Les bp du diff d'images planchonnent sur les traits fins** : une cote
  sur une carte de 90 mm change ~150 pixels sur un canvas d'1 Mpx = 1 bp,
  et le dhash 32×32 n'en voit RIEN. Le canvas 2D étant déterministe
  (QPainter, même process), le compte BRUT de pixels changés
  (img_px_changed, 0 attendu pour « identique ») est le bon outil pour les
  petits deltas — les seuils bp/dhash restent pour les changements de
  masse.

## 2026-07-17 — G2 inspection (dcode/tool, APERTURES, DRILLREPORT)

- **Les métadonnées de fab vivent à DEUX niveaux, pas un.** Le fait
  par-objet (quelle aperture M'a peint → `dcode` dans l'extra JSON de
  l'entité) et le fait par-fichier (la table des apertures → `camMeta` du
  calque). Tout mettre sur les entités duplique la table des centaines de
  fois ; tout mettre au calque perd le lien objet→aperture qu'exigera
  l'export G3. La séparation rend aussi DRILLREPORT honnête : les comptes
  se font sur les entités VIVANTES, les diamètres déclarés restent au
  calque.
- **Le commentaire vaut parfois plus que la géométrie : AMPARAMS.** Les
  primitives %AM d'un RoundedRect Altium (2 rects + 4 cercles) sont la
  vérité de RENDU mais illisibles pour un humain. Altium émet à côté
  `G04:AMPARAMS|XSize=...|CornerRadius=...|Shape=RoundedRectangle|` — la
  vérité de CONCEPTION. Parser ce commentaire (sans jamais s'en servir
  pour le rendu) donne l'inspecteur rêvé pour zéro risque géométrique.
- **Une colonne SQLite de plus = un étage de repli de plus, pas un
  remplacement.** Le SELECT à repli unique (G2) aurait envoyé les fichiers
  « d'hier » (alpha/rank/role présents, cam_meta absent) dans la branche
  legacy — pile de couches perdue en silence. Chaque génération de schéma
  garde SON étage : essayer le plus récent, descendre d'un cran à la fois.
- **Supprimer un défaut du constructeur oblige à traiter le CHARGEMENT.**
  Document() crée la couche « 0 » ; la dropper à l'import kit ne suffit
  pas — au load d'un .vkd sans ligne id 0, le défaut du constructeur
  réapparaîtrait. Le loader doit constater « le fichier n'a pas de 0 » et
  re-dropper le résidu (avec les mêmes gardes : rien ne la référence).
- **Un SELECT headless rend le GUI testable gratuitement.** Le panneau
  Propriétés ne s'inspectait qu'à la souris ; une commande SELECT de 40
  lignes + `propRows` dans query ui, et gui-smoke lit le CONTENU RÉEL du
  QTableWidget après sélection d'une pastille — l'inspecteur est verrouillé
  par le harnais, pas par une capture d'écran illisible.

## 2026-07-17 — Clôture G2 (revue adversariale)

- **Even-odd n'est PAS la sémantique des anneaux Gerber : c'est l'UNION.**
  Une pastille macro Altium (RoundedRect) flashe des anneaux qui se
  RECOUVRENT (2 rects + 4 disques de coin) ; le renderer SOLID remplit
  chaque anneau individuellement (= union à l'écran), mais le containment
  de MINDIST appliquait la parité even-odd À TRAVERS les anneaux : un
  point couvert par 2 anneaux comptait « dehors » et un perçage enterré
  dans la pastille sortait avec une clearance positive étiquetée
  « exact ». Règle : even-odd DANS un anneau (polygone simple), union
  ENTRE anneaux — et tout test géométrique doit suivre la sémantique du
  RENDERER, pas celle qui arrange l'algorithme. Attrapé uniquement parce
  que la revue a recalculé à la main depuis les fichiers bruts ; les cas
  mono-anneau passaient tous.
- **Une revue adversariale qui recalcule TOUT à la main paie.** Les 3
  rapports G2 étaient « verts partout » ; le recoupement indépendant
  (DRR/REP/coordonnées brutes du Gerber) a quand même sorti un bug majeur
  + un biais de ~2 µm (polygones inscrits, dette PCB_CAM). Les valeurs
  vérifiées à 1e-9 par un chemin INDÉPENDANT valent plus que dix tests
  qui reproduisent le calcul du code.
- **Assumé : le snap Center au point d'insertion vaut pour TOUT insert**,
  pas seulement les pastilles GBR-* (InsertEntity::snapPoints est
  générique). Dans un dessin 2D legacy, Center près d'un bloc peut donc
  accrocher son point d'insertion en plus des vrais centres de
  cercles/arcs internes. Comportement voulu (symétrie Endpoint/Center,
  utile aussi pour les blocs mécaniques) — si un jour ça gêne, restreindre
  au préfixe de nom de bloc, pas au SnapEngine.

## 2026-07-17 — G3 écrivain RS-274X

- **Ré-émettre la définition D'ORIGINE bat toute reconstruction.** Le
  renderer VikiCAD approxime les tracés à aperture RECT en bouts ronds
  (dette G1), mais l'export n'a PAS à reproduire notre rendu : il ressort
  l'ADD..R d'origine (camMeta) et gerbv peint l'exporté exactement comme
  l'original — dhash 0/1024 sur 28 couches réelles. Corollaire : le juge
  du test de vérité doit être le renderer de référence des DEUX côtés
  (gerbv vs gerbv), jamais notre propre pipeline.
- **Un comparateur point-à-point casse sur les formes RE-TESSELLÉES.** Un
  insert de pastille ronde scalé ×2 ressort en `C,d×2` ; au ré-import la
  tessellation (tolérance de corde fixe 1e-3) met PLUS de points sur le
  cercle plus grand — anneaux 61 vs 87 points pour la MÊME forme. Le
  comparateur de round-trip doit dégrader en métriques de forme (bbox à
  2 tolérances de corde près + aire à 1 %) quand les comptes de points
  diffèrent, et rester point-à-point sinon.
- **Piège Catch2/QString** : `text.contains("D15*")` matche aussi
  `%ADD10ROUNDEDRECTD15*%` — sur du Gerber, toujours ancrer les checks de
  sous-chaînes sur le préfixe de statement complet (`%ADD10...`, `%AM...*`).

## 2026-07-17 — G3 écrivain Excellon

- **Trancher un dialecte par l'expérience, pas par la doctrine.** Les deux
  candidats (METRIC,TZ 3:3 entiers vs coordonnées décimales explicites) ont
  été générés depuis le kit A réel et rendus par gerbv AVANT d'écrire le
  writer : bit-identiques à l'original Altium (dhash 0/1024, encre 0.000 pt),
  y compris avec hits modaux (87 lignes à axe omis) et picture
  `METRIC,TZ,0000.000`. gerbv honore aussi le commentaire `;FILE_FORMAT=4:4`
  (prouvé par l'absence de facteur 10 sur le rendu). Choix : décimales
  explicites — auto-descriptives (immunes au piège 3:3 vs 4:4 et LZ/TZ qui
  a justifié tout le soin du parseur G1) et pleine précision 1e-6 mm là où
  3:3 tronque à 1e-3.
- **Jamais de coordonnée sans point décimal dans ce dialecte.** Le trim des
  zéros de traîne doit garder « .0 » (`X24.0`, pas `X24`) : un entier nu
  retomberait dans l'arithmétique de suppression de zéros du consommateur —
  exactement l'ambiguïté que les décimales éliminent. Le test du dialecte
  vérifie qu'AUCUNE ligne X/Y n'est sans point.
- **Diamètre d'outil = celui du PREMIER cercle du groupe, pas le seau
  1e-4.** Grouper à 1e-4 mm mais écrire le diamètre pleine précision du
  premier cercle rencontré : un import Altium non édité (0.299974 mm issu de
  0.01181 in) fait alors le round-trip exact à 1e-6, au lieu de ressortir
  « 0.3 » arrondi. Les conversions 2:5 inch → mm tombent d'ailleurs
  TOUJOURS sur 6 décimales exactes (entier × 254 / 1e6).

## 2026-07-17 — G3 boucle CAM (export kit, PANELIZE, pont DXF)

- **Un rapport et un writer qui regardent le même objet doivent partager la
  MÊME règle d'appartenance.** DRILLREPORT ne comptait que les cercles
  tagués `tool` (importés) alors que l'écrivain Excellon exporte AUSSI les
  cercles nus des calques à rôle Drill : un trou dessiné dans VikiCAD était
  exporté mais invisible au rapport — l'incohérence est sortie en exécutant
  l'exemple de doc, pas par un test. Règle : sélection ET platage par
  défaut copiés du writer (tag sinon rôle), l'outil affiché « new ».
- **Tester « la pastille a bougé » sur un kit réel = scoper par calque.**
  Au même (x,y) qu'une pastille cuivre vivent ses flashes mask/paste ;
  « plus rien à l'ancienne position » n'est vrai QUE sur le calque édité —
  le test global échouait avec 2 inserts « restants » parfaitement
  légitimes.
- **Jamais d'arrondi-à-la-grille pour comparer deux multisets de
  flottants.** `round(x*1e4)` fait basculer de seau une valeur posée SUR la
  frontière pour un chouia de 1e-12 (vu sur les positions de flash après le
  hop DXF) : trier puis comparer par paires avec tolérance.
- **L'encre brute de gerbv est un excellent invariant d'échelle.** À DPI
  fixe et sans crop, le COMPTE de pixels encrés d'un panel 2×2 vaut 4,007×
  celui de la carte — vérité de panélisation en 6 lignes de PIL, aucune
  géométrie à recalculer.

## 2026-07-17 — Clôture G3 (revue adversariale de l'export)

- **« Légal selon la spec » ne suffit pas pour un format d'échange : viser
  le silence chez le consommateur le plus vieux.** Le %TF nu est du X2
  parfaitement valide, mais gerbv (pré-X2) loguait CRITICAL sur CHAQUE
  fichier exporté — et un fab avec un viewer daté verrait pareil. Altium
  met ses attributs X2 en commentaires `G04 #@!` précisément pour ça ;
  faire pareil coûte une ligne. Même famille : un fichier 100 % régions
  sans %AD se fait sniffer « RS-274D » — UNE aperture placeholder inutile
  suffit. Vérité opérationnelle : `gerbv --export=png` DOIT être
  stderr-silencieux sur nos exports.
- **Un token atteignable seulement par heuristique de nommage = feature
  morte, et les tests peuvent codifier le bug.** Drill-NPTH : writer et
  rapport géraient le rôle, mais aucune commande ne pouvait le POSER
  (absent des specs) et le mapping nom→rôle matchait le préfixe DRILL
  d'abord. Deux tests existants VERROUILLAIENT le mauvais comportement
  (`role == "Drill"` sur le calque NPTH). Leçon double : pour chaque
  valeur d'un enum de comportement, un test doit passer par le CHEMIN
  UTILISATEUR (commande), pas par le setter interne ; et matcher les
  préfixes du plus long au plus court.
- **Un export multi-fichiers qui échoue au milieu doit nettoyer derrière
  lui.** 9 fichiers .G** plausibles + pas de .TXT = un kit sans perçages
  expédiable par un script qui ignore le JSON. Supprimer les fichiers
  déjà écrits ET les nommer dans l'erreur ; l'alternative temp+rename est
  plus lourde pour zéro gain ici.
- **Toute commande multiplicative (grille, réseau) mérite un plafond
  chiffré.** PANELIZE 100 100 (faute de frappe plausible) = 23 M
  d'entités clonées dans UNE transaction : minutes de gel + Go de RAM +
  undo qui double. Mesurer le coût unitaire (4.5 µs/entité), choisir un
  cap qui reste undoable (2 M ≈ 9 s), refuser AVANT la transaction avec
  le calcul affiché.
- **La parité CLI/GUI se teste sur les REFUS, pas seulement les succès.**
  `export FILE.vkd gerbers` (slash oublié) : la GUI répondait « unsupported
  format », le CLI écrivait un DXF nommé « gerbers » avec ok:true — le
  fallback « tout le reste est du DXF » datait d'avant les cibles
  répertoire. Chaque canal doit refuser les mêmes entrées.

## 2026-07-23 — Clôture « point de départ 3D » (revue adversariale)

- **Piège .scr/.vks des étapes optionnelles (corrigé)** : une commande en
  attente à une étape « mot-clé optionnel » (`WORKPLANE XZ` à son prompt
  `[OFFSET]`) consommait le premier token de la LIGNE SUIVANTE comme
  terminaison, puis `finishCommand` jetait le reste de la ligne —
  `WORKPLANE XZ` + `RECT ...` produisait un document VIDE avec ok:true.
  Correctif générique : `Step::doneRepush()` — la commande déclare le
  token non consommé et le CommandProcessor re-soumet ce token + la
  suite comme NOUVELLE ligne de commande (la vraie sémantique AutoCAD).
  Règle : toute étape optionnelle à mot-clé doit repousser les tokens
  étrangers, jamais les avaler. Reproduit par test AVANT le fix
  (test_workplane_planes.cpp, 4 sections : .vks, barre de commande,
  chaînage sur une ligne stricte).
- **« Vérifié par gui-smoke » doit se grep-er dans gui-smoke.** Le flux
  sketch-sur-face (beginSketchOnFace) était déclaré couvert par le
  harnais alors qu'aucune ligne ne l'exerçait (le harnais ne testait que
  SKETCH NEW/CLOSE). Avant d'écrire « le harnais couvre X » : `grep X
  scripts/gui-smoke.sh`. Couverture ajoutée : phase « face: » (pick3d
  centre → sketchface → CIRCLE → SKETCH CLOSE → EXTRUDE, volume/bbox
  du cylindre prédits à la main).

## 2026-07-23 — Clôture « ergonomie sketch » (revue adversariale)

- **PIÈGE IPC préexistant (documenté, non corrigé — hors périmètre) : une
  commande laissée EN ATTENTE d'un prompt avale les `exec` suivants.**
  En session `connect`, un `exec "SELECT 0,0 40,20"` qui échoue au parsing
  des ids (« expected entity ids ») laisse la commande ACTIVE ; les lignes
  exec suivantes (`RO 1 0,0 30`, `SELECT 1`…) sont consommées par le
  prompt EntitySet pendant, silencieusement. Déblocage : `connect exec ""`
  (Enter vide = Finish). Règle de pilotage IPC : après un exec qui peut
  laisser un prompt ouvert, vérifier le message retourné et envoyer un
  `exec ""` avant la ligne suivante. (Le chemin submit() strict/non-strict
  est antérieur au lot ergonomie ; à corriger un jour côté processeur —
  un exec strict ne devrait jamais laisser une commande suspendue.)
- **La liste d'un refus ambigu parle en noms CANONIQUES** : `D` liste
  ERASE (atteint via son alias DEL) et TEXTEDIT (via DDEDIT). C'est le
  design voulu (les préfixes matchent aussi les alias, dédupliqués par
  commande canonique) et rien n'est lancé, mais si un utilisateur s'en
  étonne, la réponse est là. Corollaire vérifié : `G` lance GEAR et `W`
  lance WORKPLANE — préfixes uniques légitimes.
- **Un commentaire de test avec des chiffres « à la main » se recalcule
  comme le test lui-même.** La revue a attrapé un `|v3|² = 724` qui vaut
  925 dans test_rect_profile.cpp — les assertions étaient justes, le
  commentaire non. Un chiffre faux en commentaire coûte une heure au
  prochain lecteur qui refait le calcul.
