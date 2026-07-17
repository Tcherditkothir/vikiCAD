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
