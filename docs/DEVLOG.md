# VikiCAD — DEVLOG

Journal de développement. Une entrée par session de travail significative ; une entrée obligatoire à chaque clôture de jalon.

---

## 2026-09-24 (bis) — Issue GitHub #13 : SWEEP perdait la moitié du tube à un coude franc

Constat de l'agent dessinateur : ses tubes le long d'une polyline 3D (marqueurs
anatomiques, ~45 segments) sortaient à ~150 % du volume nominal et Ø 4,2 mm au
lieu de 2,8. Il accusait `Manifold.rotate([gx, gy, gz])` de composer des
rotations d'axes au lieu d'une rotation A L'AXE (cross-product), et demandait
la note documentaire + le contournement par `Manifold.transform(m)` avec base
orthonormale.

**Vérifié d'abord, des deux côtés, avec la vraie bibliothèque** (le venv
`~/.venvs/mesh` porte manifold3d 3.5.3 + trimesh + numpy) : l'accusation est
FONDÉE. La docstring du wrapper dit de son propre `rotate(v: Doublex3)` :
« *[X, Y, Z] rotation in degrees … From the global reference frame, a model
will be rotated in x-y-z order* » — c'est un EULER x-y-z, pas un axe-angle. Le
vecteur de Rodrigues `axe × angle` n'aligne donc que si la rotation se trouve
être mono-axe. Mesuré (axe du cylindre retrouvé par covariance du maillage, sur
`cylinder(20, 1.4, 1.4, 32, center=True)`) : **0,0000° d'erreur** pour toutes
les directions du plan YZ (le cas « facile », qui masque le piège), mais
**1,78° / 3,90° / 6,09° / 6,71°** pour des directions obliques quelconques —
contre **≤ 0,0043°** par `transform(m)` à base manuelle. À 6,7° sur un segment
de 50 mm, le disque de bout se déplace latéralement de `50·sin(6,7°) ≈ 5,8 mm`,
soit plus d'un demi-rayon : ce sont les « renflements aux coudes », et le
~150 % de volume livré. La note demandée est écrite (AGENT.md §7, tableau des
erreurs mesurées + le patron `transform` en 8 lignes de numpy).

**Et le bug de VikiCAD, trouvé en reproduisant son besoin avec nos moyens
natifs** — puisque la réponse utile à son besoin reste SWEEP, pas manifold3d.
Le tube natif d'un CHEMIN DROIT est exact (`volume=307.9` pour un r=1,4 sur
50 mm : π·r²·L au chiffre près), et un chemin en ARC l'est aussi (242,0 pour un
nominal de 241,8). Mais une `PLINE` à **coude franc** — le cas même du
rapporteur — rendait `volume=153.9` au lieu de 307.9 : **exactement la moitié**,
une jambe sur deux, en répondant `ok:true` comme un succès normal. Balayage des
longueurs de jambes : 25+25 → 153,9 ; 25+50 → 153,9 ; 50+25 → 307,9. Le solide
OCCT est pourtant **valide et sa coque fermée** (`BRepCheck_Analyzer` OK) —
seul le volume dénonce la perte.

**Cause racine (sonde OCCT de 10 lignes, liée directement, zéro build du
projet) :** `BRepOffsetAPI_MakePipe` sur une spine dont la direction CASSE
construit une forme valide qui ne contient que la PREMIÈRE jambe. Mesuré côte
à côte sur la même géométrie (disque r=1,4 le long de `0,0 → 0,25 → 25,25`) :
`MakePipe` global → 153,938 (50,0 %) ; `MakePipe` par jambe tangent-continue +
`BRepAlgoAPI_Fuse` → 307,091 (99,7 %) ; les trois modes de
`MakePipeShell` → pires (71,9 % / 71,2 % / 33,3 %, et `TopAbs_SOLID` absent,
coque ouverte). Sur une jambe droite ou un arc, les deux voies donnent
exactement pareil (100 %) : le découpage ne dégrade rien.

**Correctif :** `sweepProfile` découpe la spine en jambes **tangent-continues
maximales** (`tangentLegs`, tangente d'arête par arête à 1e-6), sweep une jambe
à la fois — chaque profil étant réorienté sur la tangente ET le point de départ
DE SA jambe, pour couvrir le coude — puis fusionne. Résultats mesurés : coude
90° **304,2/307,9 (98,8 %)**, escalier 4 jambes × 12 mm **284,6/295,6 (96,3 %)**,
escalier 3 jambes × 25 mm **454,5/461,8 (98,4 %)**, chemin droit 307,9 et arc
242,0 inchangés. Le léger déficit des coudes est normal (l'angle est taillé,
pas rempli). Le tube est désormais **watertight** (coude : 8 faces, 14 arêtes,
arête ellipse au pli) là où il ne l'était pas.

**Pourquoi `requireSolid` n'a rien vu :** il ne rejette que le volume NUL
(`> 1e-12`). Un volume PARTIEL — la moitié d'un tube — passe tous les gardes.
C'est la classe « un résultat plausible mais faux », la plus coûteuse : elle
voyage loin. Le critère de détection est documenté (un tube à ~50 % de π·r²·L
= le sweep s'est arrêté au premier coude).

**Preuves :** `tests/test_sweep_loft.cpp` (+3 cas tag `[issue13]`, 15
assertions) — coude 2 jambes (volume > 95 % du nominal + coque unique fermée),
escalier 4 jambes, et un arc (non-régression). **Vérifié dans les deux sens :
les cas coude/escalier ÉCHOUENT sans le correctif, avec exactement les volumes
du bug (153,938 et 147,781)**, et passent avec. Suite complète **473 cas /
9 289 assertions, 0 échec** (5 skips GL sans display) ; `gui-smoke.sh` **ALL
GREEN (378 checks)** avec 6 nouvelles assertions `elbow:`. Docs : AGENT.md §7
(deux entrées : SWEEP coudé + `Manifold.rotate` vs `transform`), LESSONS.md.

---

## 2026-09-24 — Issue GitHub #12 : une commande qui s'annule en silence répondait ok:true

Constat de l'agent dessinateur (scene SNC PSY1014) : ses tubes de sillons
(r ~1,4 mm, courbe de 85 mm) sortaient en disques de 100 mm sur 1,4 mm
d'épaisseur, 30-60x trop gros, et seule la vérification numérique l'a
attrapé. Le rapport accusait l'ordre des arguments du wrapper Python
`manifold3d` (`Manifold.cylinder(height, radius_low, ...)` — la HAUTEUR
d'abord, le rayon ensuite), et demandait soit un avertissement dans
AGENT.md, soit une commande utilitaire TUBE.

**Vérifié d'abord : VikiCAD n'a rien à voir avec manifold3d** — aucune
dépendance, aucun script Python dans le dépôt. Le piège est dans le
générateur maison de l'agent, hors du logiciel. Mais en reproduisant son
besoin (un tube de rayon 1,4 le long de 85 mm) avec les moyens natifs, un
second piège est apparu, celui-là BIEN à nous : `SWEEP 1 2` — le profil et
le chemin sur une seule ligne — répondait `{"ok":true}` avec un document
INTACT et ZÉRO message. La commande s'était annulée en silence.

**Cause racine :** le glouton `EntitySet` (AGENT.md §2 règle 5) avale tous
les tokens numériques de la ligne et les donne au PREMIER stage de
sélection ; le second stage (le chemin) meurt de faim, reçoit un Finish
implicite et retourne `Step::cancelled()`. `drive()` traitait alors
l'annulation comme un succès (`{true, {}, false, {}}`) — indistinguable
d'une réussite pour un appelant headless. Le refus explicite existait déjà
pour le cas VOISIN (issue #2 : un token non-numérique au mauvais moment),
mais pas pour la famine d'un stage. Mesuré ensuite commande par commande :
**trois** tombaient dans le silence — `SWEEP` (profil→chemin),
`EXTRUDE … CUT` (cible→profils) et `PATTERN3D` — tandis que `MATE`,
`SPLITFACE` et `CHAMFEREDGES` consommaient leurs ids autrement (pas de
second stage affamé) et que `LOFT` refusait déjà en nommant sa cause.

**Correctif (garde de mode strict, dans `CommandProcessor`) :** `drive()`
retient désormais OÙ une commande s'est annulée (son nom + le prompt
courant) ; si elle s'annule sans avoir laissé UN SEUL message et qu'on est
en mode strict (headless : `--exec`, `--run`, IPC `exec`), `submit()`
transforme le faux succès en erreur nommée :

```
SWEEP cancelled itself with nothing done and no explanation (waiting at
"Select path:"). If one line fed it two selection stages at once, give
each stage its own line — a .vks script (AGENT.md §2 rule 5).
```

Deux garde-fous voulus : **le mode interactif reste muet** (en GUI, une
annulation EST la réponse de l'utilisateur — un Échap ne doit pas lever de
dialogue) ; et **une commande qui s'est expliquée est laissée tranquille**
(LOFT refuse déjà en nommant sa cause — le garde ne réécrit pas son
diagnostic).

**Ce qui a été répondu au demandeur, en plus du correctif :** la note
demandée est dans AGENT.md (§7 gotchas — l'ordre `cylinder(height, radius)`
de manifold3d, la mesure des disques 100 mm/1,4 mm, la même famille de
piège ailleurs), ET la vraie réponse à son besoin — **SWEEP natif construit
le tube directement** : `CIRCLE` pour la section + `LINE`/`ARC`/`PLINE`
pour l'épine + un script `.vks` de deux lignes, sans STL, sans MESH2SOLID,
sans bibliothèque tierce. Vérifié : r=1,4 sur 50 mm donne
`volume=307.9 mm3` — exactement pi·r²·L — et un chemin COURBE donne
`773.8 mm3` pour l'arc de 125,66 mm, encore pi·r²·L. Le critère de
détection est écrit dans le guide : un tube dont le volume s'effondre et
dont la bbox explose est un DISQUE.

**Preuves :** `tests/test_silent_cancel.cpp` (6 cas, tag `[issue12]`, 29
assertions) — refus bruyant du une-ligne, construction du tube par le
script documenté (volume pi·r²·L), forme `SWEEP <chemin>` après
pré-sélection toujours bonne, `EXTRUDE CUT` sur une ligne désormais refusé,
commande bavarde (LOFT) non réécrite, mode interactif muet. Suite complète
**470/470 après commit** (464 avant + les 6 cas) ; `gui-smoke.sh` **ALL
GREEN (373 checks)** avec les phases `silent cancel:` (4) + `tube:` (5).
Docs : AGENT.md §2 règle 5, §7 gotchas (deux entrées), LESSONS.md.

## 2026-09-24 — Issue GitHub #9 : les PRODUCT du STEP perdent leur nom quand le nom vit au top-level du JSON

Constat v4 PSY1014 (vivi-dessinateur) : l'export STEP du .vkd assemblé par
SQL (112 solides) ne nommait que 27 PRODUCT ; les 85 solides ajoutés par le
script tombaient en `SOLID`/`SHELL`/`COMPOUND` génériques, alors que
`query entities` affichait bien un `component` pour les 112.

**Cause racine (lue dans les DEUX bouts du format, puis prouvée sur les
fichiers) :** le champ `component` d'un solide vit CANONIQUEMENT dans
`geom.component` (c'est `SolidEntity::geomToJson` qui l'écrit là, et
`geomFromJson` qui le relit de là). Le script qui a assemblé le v4 a posé
le nom des nouvelles entités au TOP level de l'entité JSON, à côté de
`color` — emplacement que la lecture ignore : `geomFromJson` écrase
`component` par le `geom.component` ABSENT (= vide). L'entité vit donc
sans nom ; le writer STEP (qui lit `solid->component`, rien d'autre)
tombe sur vide, et l'auto-naming XCAF d'AddShape estampille
`SOLID`/`SHELL`/`COMPOUND`. Preuve par position : les 27 noms du fichier
exporté égalent EXACTEMENT l'ensemble des `geom.component` du document
(noms hérités des STEPs d'origine : `neocortex-decim`, `fornix-wt`...),
pas les `component` top-level — et `DESCRIBE 34` (champ VIVANT) n'affiche
rien là où `query entities` (JSON brut avec l'extra bag) montre un nom.
L'issue pensait « le writer ne s'en sert pas » ; le champ n'arrivait
jamais à l'entité.

**Correctif : tolérance de lecture, pas de changement au writer.**
`Entity::fromJson` appelle maintenant un second pass `postFromJson(obj)`
(hook virtuel, no-op par défaut) avec l'objet RACINE ;
`SolidEntity::postFromJson` adopte le `component` top-level SEULEMENT si
le champ canonique est revenu vide — quand les deux niveaux portent un
nom, `geom.component` gagne (les fichiers v3 gardent leur verdict). Le
premier `save` converge le fichier (le nom migre dans `geom.component`).
Aucun changement au writer STEP ni au schéma : un nom au top-level reste
aussi dans l'extra bag, inoffensif.

**Preuves :** `tests/test_solid_component.cpp` (5 cas) — survie du nom
top-level au load, canonicalité de `geom.component` quand les deux
niveaux coexistent, anonymat préservé, round-trip .vkd avec hot-patch
SQLite à l'identique du script PSY1014 (puis convergence au re-save), et
export STEP dont le PRODUCT porte le nom (jamais `PRODUCT('SOLID'`).
Sur le FICHIER RÉEL snc-psy1014-v4.vkd : avant = 27 noms / 85 génériques ;
après = 112 PRODUCT, 112 uniques, 0 générique (extrait + comparaison
JSON dans le livrable inter-agents).

**Piège du diagnostic :** `query entities` sérialise le JSON BRUT
(`toJson`, extra bag compris) — il montre un nom que l'entité vivante ne
porte pas. C'est exactement ce qui a fait conclure l'issue que le champ
était « bien là ». Règle : pour juger de l'état d'une entité, `DESCRIBE`
/ `query describe` (le champ vivant), pas le JSON brut.

**Vu au passage (hors périmètre #9, préexistant) :** le round-trip STEP
du v4 ne ramène que 41/112 solides — les shells OUVERTS de MESH2SOLID
sortent en `SHELL_BASED_SURFACE_MODEL` et `documentFromXcaf` ne garde
que les `TopAbs_SOLID`. Même chiffre sur l'export d'avant le correctif :
pas une régression. Candidat issue séparée.

**État :** ctest 464/464 verts (457 + 5, avec le lot #11 de la session
sœur) ; gui-smoke 351/353 — les 2 échecs restants sont la phase
`cli timeout:` de #11 en cours chez l'autre session (binary mid-edit),
pas de chemin #9 dedans ; refdiff/expdiff verts en isolation. Issue
fermée après le commit.

---

## 2026-09-24 — Issue GitHub #7 : insertstep/MESH2SOLID IPC — le reply ne paie plus le refresh, et il dit le coût

Constat v4 PSY1014 (vivi-dessinateur) : `insertstep` suivi de
`MESH2SOLID` sur les maillages BP3D/Pitt du CNS v3 (~6k+ faces) dépasse
la fenêtre d'attente du canal IPC, alors que le même enchaînement passe
par le chemin offline (`import` + `--run`). Repro de cette séance :
sphère binaire 19 208 triangles (sous le plafond 20k), `MESH2SOLID 1 2`
sur deux exemplaires = `E_TIMEOUT` à 10,0 s pile ; le même convertit en
4,7 s offline.

**Cause racine (lue dans le code, côté GUI cette fois) :**
`insertComponentFile()` finissait par `refreshAfterAssemblyChange()` —
`refreshFrom` rebuild complet des structures de sélection — AVANT que le
reply parte ; le serveur RPC étant synchrone, tout ce temps est un
silence socket pour le client, qui donne au bout de sa fenêtre (le
plafond 10 s a sauté dès ~38k triangles en un exec, à 5 s par
conversion).

**Correctif (gui/MainWindow.{h,cpp}) :** le verbe IPC `insertstep` passe
`refreshView=false` et DIFFÈRE le rafraîchissement sur le timer
single-shot zéro-délai du correctif #6 (coalescé par rafale) ; le timer
choisit maintenant l'action via `m_ipc3dSyncRefreshAsm` :
`refreshAfterAssemblyChange()` (insertstep) ou `sync3DView()` (exec) ;
`flushIpc3dSync()` honore le flag, donc query/pick3d/viewdir/screenshot
ne peuvent jamais lire une scène plus vieille que le document.
**Et le reply est enrichi** (la boussole demandée par l'issue) :
`inserted[{id,name,triangles}]` + `triangles` total — l'agent connaît le
coût du futur MESH2SOLID (sewing ≈ une face par triangle, plafond 20 000)
AVANT de l'envoyer, et choisit `--timeout` généreux ou le chemin offline
au lieu d'un timeout à l'aveugle. Le volet fenêtre réglable
(`connect --timeout` / `VIKICAD_CONNECT_TIMEOUT`) est livré par la
séance #10 (entrée ci-dessus).

**Mesures réelles (build debug) :** insertstep 19,2k tri = 0,04 s avec le
payload (avant : 0,3 s sans payload) ; MESH2SOLID 19,2k tri par IPC =
4,84 s (dans la fenêtre 10 s par défaut) ; sur le cas 38,4k tri, le
reply nomme 38 416 triangles et l'agent arbitre en connaissance de cause.

**Vérification par la suite officielle :** phase gui-smoke `meshimp`
étendue de 8 checks (insertstep : reply ok, nom du composant, triangles
12/12, entité posée, visible au describe après flush, ERASE). Passage
19/19 dans le run complet de la séance #11 (16 h — partagé : nous
travaillons à quatre sur le même arbre, coordination 15:45). Les runs de
cette séance sont bruités par les quatre sessions en parallèle sur le
même socket `vikicad` (échecs en cascade E_CONNECT/count décalés,
races documentées dans le rapport livrable) ; ctest 457/457.

---

## 2026-09-24 — Issue GitHub #11 : le verbe CLI `open` n'acceptait que le .vkd

Constat v4 PSY1014 (vivi-dessinateur) : `$CLI open file.stl --save-as out.vkd`
échouait avec `not a VikiCAD file (no entities table): file.stl`, alors que
`$CLI import file.stl --save-as out.vkd` réussissait et que la GUI ouvrait le
même fichier par File>Open. Deux verbes, un seul fichier, deux verdicts — le
piège classique pour un agent qui prend `open` par réflexe.

**Cause racine :** deux chemins d'ouverture au lieu d'un. `cmdNewOrOpen`
(nouveau comme `open`) appelait `NativeStore::load` en dur — le `.vkd` natif
seulement. La GUI, elle, portait sa propre table de dispatch par extension
dans `MainWindow::loadFile` (écrite au chantier « pourquoi je ne peux pas
ouvrir un dwg direct ? »), et le verbe CLI `import` sa propre copie encore.
Trois implémentations de la même politique, dont deux seulement connaissaient
les formats importés.

**Correctif : UNE table de dispatch partagée.** Nouveau module
`core/io/FileOpen.{h,cpp}` (`openFileAny()`), reprise fidèle de l'ordre et
des règles de `File>Open` : répertoire = kit Gerber ; `.vkd` = store natif ;
`.dxf`/`.dwg` ; `.step`/`.stp` ; `.igs`/`.iges` ; `.stl` ; `.brd`/`.sch`/
`.lbr` ; fichier Gerber/Excellon isolé (sniff de CONTENU) ; et en repli le
store natif, pour que le comportement historique du `.vkd` renommé ou sans
extension ne casse jamais derrière le nouveau routage. Le marquage des
composants (nom de fichier en repli pour STEP/IGES quand le fichier n'en
porte pas, systématique pour STL) est reproduit à l'identique, et
`OpenFileResult` remonte les compteurs de chaque importeur.

`cmdNewOrOpen` utilise désormais `openFileAny()`. Trois conséquences
voulues :

1. `open f.stl` et `import f.stl` produisent le MÊME document (même routage,
   même importeur, mêmes marquages) ;
2. la réponse JSON de `open` gagne un champ `dispatch` (l'arme prise :
   `vkd` `dxf` `dwg` `step` `iges` `stl` `eagle` `gerberkit`) et **reprend
   les compteurs du verbe `import`** (`solids` `triangles` `imported`
   `skipped` `files` `entities` `layers` `warnings`…) — un agent qui lit
   `open` n'a plus à deviner ce qui est arrivé ;
3. garde-fou ajouté : `open f.stl --save` est REFUSÉ (`E_ARGS`), parce
   qu'un `--save` nu écraserait le fichier SOURCE avec des octets `.vkd`.
   Le message nomme le verbe correct (`--save-as OUT.vkd`). Pour un `.vkd`
   ouvert nativement, `--save` reste légitime et inchangé.

**Doc (le point précis de l'issue) :** la ligne `open` du §1a ne disait que
`FILE.vkd`, pendant que la ligne du §1b décrivait le dispatch par extension
comme s'il était propre à l'IPC/GUI. Les deux lignes nomment maintenant le
routage partagé, et une note dédiée au §7 (« One routing for every open
channel ») explique la distinction `open` (prendre un fichier) vs `import`
(verbe explicite) — c'est l'ambiguïté documentaire qui a produit le ticket.

**Preuves :** 7 cas Catch2 neufs dans `tests/test_file_open.cpp` (tag
`[issue11]`, 120 assertions) — routage `.vkd`/`.stl`/`.step`/`.dxf`/kit
Gerber/fichier fab isolé, repli natif d'une extension inconnue, messages et
compteurs, marquage des composants (STEP anonyme = stem du fichier, STEP nommé
= nom conservé), et l'égalité `open`/`import` sur le même STL. ctest
464/464 (457 avant + 7). Phase gui-smoke « open dispatch: » ajoutée,
exécutable sans GUI (fixtures auto-construites), + suite complète relancée.

**Piège rencontré :** le writer STEP de vikiCAD estampille TOUJOURS un nom de
PRODUCT (`SOLID` pour un corps anonyme) — impossible de fabriquer un STEP
« sans nom » avec notre propre export pour tester le repli du stem. La fixture
du test part donc d'un export réel dont les champs PRODUCT sont blanchis :
c'est ce à quoi ressemble la sortie d'un exporteur brut, et le repli s'y
vérifie pour de vrai. À ne pas confondre avec l'issue #9 (le nom du PRODUCT
côté writer) — chantier distinct.

---

## 2026-09-24 — Issue GitHub #10 : `connect open` renvoie un faux E_TIMEOUT sur les .vkd lourds

Constat v4 PSY1014 (vivi-dessinateur) : `connect open` d'un .vkd de
269 Mo (112 solides OCC, néocortex ~19 000 facettes ×2) répond
`E_TIMEOUT / "no response"` après ~10 s alors que l'ouverture finit par
réussir dans la GUI — le modèle est correct (count=112 au `query
entities`). Faux négatif pur.

**Cause racine (lue dans le code, les deux bouts de la socket) :** le
client CLI (`cmdConnect`) lit la réponse avec `waitForReadyRead(10000)` —
10 s de SILENCE socket. Le serveur RPC (`gui/ipc/RpcServer.cpp`) est
strictement synchrone : `handleRpc("open")` appelle `loadFile`, qui ne
RETOURNE qu'après `NativeStore::load` (les BREP base64 de 112 solides
désérialisés) + `adoptDocument` (rebuild des panels, zoomExtents). Le
premier `waitForReadyRead` porte donc TOUTE la durée du chargement : sur
un gros fichier, le client abandonne pendant que la GUI travaille encore.
La commande `query entities` qui suit (~60 s de BREP sérialisé) passait
parce que ses chunks arrivent vite (socket vivante, budget réarmé à
chaque chunk).

**Correctif (côté client, une seule méthode) :** budget de SILENCE
paramétrable `connect --timeout MS` + variable `VIKICAD_CONNECT_TIMEOUT`
(env gagne sur le flag). Le budget se réarme à chaque chunk — un
transfert vivant n'est jamais tué, une GUI muette aborte après la
fenêtre. Défaut inchangé : 10 s (aucun comportement interactif ne
change). Refus propres `E_ARGS` pour valeur manquante/mal formée et
option `--` inconnue ; le message E_TIMEOUT nomme les deux leviers.
Choix design : PAS de réponse async (ok immédiat + poll) — un `ok:true`
immédiat ferait croire à un agent que le document chargé est le nouveau
ALORS QUE `query` pendant l'ouverture lirait encore l'ANCIEN document ;
la fenêtre large garde le contrat « la réponse = le travail fini ».

**Piège du lot (attrapé par les cas CLI, pas par les yeux) :** premier
essai avec `rest = args.mid(1)` — mais `args[0]` EST la méthode (l'ancien
code lisait `args.first()`) : le décalage faisait disparaître le dispatch
et passer `--timeout` nu à travers. Corrigé en `rest = args` (méthode en
rest[0]) ; les 5 cas E_ARGS/E_CONNECT du harnais verrouillent le
découpage dans les deux positions du flag.

**État :** ctest 452/452 verts ; phase gui-smoke `cli timeout:` 5/5
(exécutée en isolation — la phase ne parle PAS au GUI, donc validable
headless) ; suite gui-smoke complète non relancée (GUI en unité systemd
pas (re)démarrée dans cette séance — le harnais la démarre tout seul).
Live sur le fichier de l'issue (269 Mo, --timeout 900000) : À FAIRE à la
prochaine fenêtre GUI.

---

## 2026-09-24 — Issue GitHub #8 : MESH2SOLID ne réoriente pas les faces (volume signé négatif)

Demande inter-agents de vivi-dessinateur (constat v4 PSY1014) : un STL fermé
produit par trimesh (union **concaténée** de cylindres + coquilles icosphere,
sans booléen) ressort de `MESH2SOLID` avec un **volume signé négatif** stable
(−353 mm³ sur les tubes/scissures). Le workaround trouvé par lui était une
union booléenne `manifold3d` en amont.

**Repro d'abord, hors dépôt** (`/tmp/vp8`, quatre STL générés à la main) :

```
$ vikicad-cli import cube-inv.stl --save-as cube-inv.vkd
$ vikicad-cli open cube-inv.vkd --exec "MESH2SOLID 1" --save-as cube-inv-solid.vkd
  messages: ["solid 1: 12 triangles sewn into 12 faces (closed solid)"]
$ vikicad-cli query cube-inv-solid.vkd --describe
  volume: -1000.0000000000002        # le cube fait 1000 mm3 par enroulement sortant
```

Le même cube, 12 facettes inversées : géométrie identique au point près,
orientation opposée, volume **−1000**. Le report dit « closed solid » — donc
rien, ni dans le message ni dans les chiffres, ne signalait l'anomalie.

**Deux défauts distincts, mesurés avec une sonde OCCT autonome** (probe.cpp,
`/tmp/vp8` — chemin de sew copié de `meshToSolid`, puis chaque remède candidat
essayé un par un) :

1. **Enroulement entrant jamais normalisé.** Le sew transmet la winding d'entrée
   telle quelle. `BRepBuilderAPI_MakeSolid` en fait un solide « valide » dont
   `BRepGProp::VolumeProperties` est négatif. C'est le défaut rapporté — et il
   n'a rien à voir avec l'enchevêtrement des nappes : il suffit d'un maillage
   inversé, même parfaitement fermé et propre.
2. **Coques multiples perdues.** Le code ne regardait qu'un `TopAbs_SHELL` de
   premier niveau. Un maillage issu d'une union concaténée se coud en
   `TopAbs_COMPOUND` de plusieurs coques fermées disjointes (aucun sommet
   partagé) : la condition `sewn.ShapeType() == TopAbs_SHELL` échouait, le
   `closed` restait faux, et le report disait « open shell » alors que **toutes**
   les moitiés étaient étanches. Aucune n'était convertie en solide.

Le diagnostic de l'issue (nappes enchevêtrées) était donc partiellement juste :
le vrai déclencheur est l'orientation, et le symptôme « open shell » signalait
un second bug que l'issue ne nommait pas.

**Pourquoi l'orientation ne peut pas se déduire d'elle-même.** Une coque fermée
est une surface orientable à deux côtés : la seule donnée géométrique ne dit pas
quel côté est « dehors ». Le critère déterministe et bon marché est donc le
volume signé — négatif = faces tournées vers l'intérieur. C'est exactement ce que
`trimesh.repair.fix_normals` fait pour l'appelant, comme l'issue le demandait.

**Correctif** (`core/io/StlIo.cpp`, parcours **coque par coque**) :

1. coque fermée → solide (`MakeSolid`) ;
2. si `BRepGProp::VolumeProperties < 0` → `BRepLib::OrientClosedSolid`, avec un
   repli explicite (`Reversed()` sur chaque face, coque reconstruite) pour le
   cas rare où OCCT refuse ;
3. **tout** est rassemblé dans un `TopoDS_Compound` — solides et coque
   réellement ouvertes (un maillage troué reste de la géométrie légitime, jamais
   sacrifié) ;
4. une seule coque ressort nue (pas d'enveloppe compound), forme que voyaient
   déjà tous les consommateurs et tous les tests antérieurs.

`MeshToSolidResult` gagne trois compteurs (`solids`, `openShells`, `reoriented`)
et `closed` garde son sens mais devient vrai seulement si **toutes** les coques
ont abouti. Le message nomme désormais le nombre de corps — `2 closed solids`
au lieu d'un « open shell » trompeur — et annonce le retournement quand il a
lieu, pour ne pas « réparer » silencieusement un maillage inversé.

**Preuves.** Sonde de stratégie validée 32/32 fichiers avant d'écrire une ligne
de code de production. Après correctif, sur les **28 maillages trimesh réels**
du livrable SNC de vivi-dessinateur (`livrables/20260924-1020-zcode-snc-v3-complements/dashboard-meshes/`)
— le cas exact de l'issue :

```
Total=28  positifs=28  negatifs=0  echecs_ou_anomalies=0
```

Deux résultats qui montrent l'enjeu réel au-delà de la reproduction minimale :

| fichier | avant | après |
|---|---|---|
| `tronc-cerebral.stl` (5992 tri, 24 coques) | 19 104.76 mm³ | **21 465.58 mm³** |
| `arachnoide.stl` (19000 tri, 4 coques) | 2 579 743.52 mm³ | **2 579 782.83 mm³** |
| `dure-mere.stl` (19000 tri, 3 coques) | 3 010 220.23 mm³ | **3 010 335.97 mm³** |

Sur `tronc-cerebral`, **14 des 24 coques** étaient soustraites : le volume
« plausible » d'avant était le résultat de soustractions parasites, pas une
mesure. Un volume signé négatif est spectaculaire ; un volume positif mais faux
ne l'est pas, et le correctif attrape les deux.

**Réserve honnête.** `BRepCheck_Analyzer` déclare certains de ces maillages
invalides — mais l'invalidité est **pré-existante dans le chemin de production**
(elle vient du sew brut, vérifié sur le code d'avant correctif), pas introduite
par lui. Le correctif ne l'aggrave pas et ne prétend pas la guérir : le sew d'un
maillage en facettes produit une coque à une face par triangle, et les
tolérances d'arêtes y sont limites. Documenté à LESSONS.md plutôt que masqué.

- **Tests** : 4 cas de régression ajoutés (`tests/test_stl_import.cpp`, tag
  `[issue8]`), chacun sur une fixture STL **construite octet par octet** dans le
  test (enroulement inversé, deux coques disjointes, winding mixte, libellé du
  message) — pas de dépendance à un exporteur dont le comportement pourrait
  changer. Le test de non-régression existant (« volume exactement 1000 »,
  « 12 faces ») reste vert inchangé.
- **Doc** : AGENT.md (gotcha MESH2SOLID), CHANGELOG.md, DEVLOG.md (ici),
  LESSONS.md (le piège de l'orientation et de l'invalidité pré-existante).
- **Clôture** : issue #8 fermée sur GitHub après suite verte + gui-smoke vert.

---

## 2026-09-24 — Issues GitHub #1-#4 (demande inter-agents de ZCode)

Quatre issues ouvertes par vivi-dessinateur pendant le dessin du SNC
traitées d'un lot, chacune vérifiée par un repro headless avant/après
(tous les extraits de messages ci-dessous sont des sorties réelles du
CLI build/debug) :

- **#1 — contrat une-invocation-une-séquence + erreurs qui nomment la
  cause.** Le vrai comportement est documenté (une invocation = un
  document neuf, état jamais partagé entre invocations) dans
  AGENT.md §1a ; `SKETCH CLOSE` sans sketch ouvert explique maintenant
  TOUT (invocation fraîche, --exec chaînés, --run script.vks, SKETCH
  Open dans la GUI) au lieu d'un sec « no sketch is open ». Pas de
  mode session : le contrat one-shot + --run couvre le besoin réel
  (l'agent de dessin a fini son SNC avec un seul --run).
- **#2 — erreurs qui nomment la ligne fautive + séparateur `;`.** La
  réponse E_EXEC inclut la ligne (déjà) — et la vraie morsure, le
  « expected entity ids » nu, dit maintenant QUELLE commande attend
  encore et à quel prompt : « expected entity ids for command EXTRUDE
  (waiting at "Select profiles:") — put the ids on their own line, or
  a blank line before the next command » (CommandProcessor, gulp
  EntitySet vide). Et un `;` dans un --exec sépare des commandes
  complètes : `--exec "RECT 0,0 40,30; EXTRUDE 10 1"` en lance deux,
  première erreur aborte avec le segment fautif dans le message.
  AGENT.md §1a + §2-5 mis à jour.
- **#3 — SETCOLOR <ids> <#RRGGBB|nom> [alpha].** Commande
  first-class dans CommandsAssembly (registre aux côtés de TRANS) :
  hex 6 chiffres ou noms de base (red/green/blue/…/orange), alpha
  0-100 = pourcentage TRANSPARENT (clampé), pickfirst (SELECT 3 puis
  SETCOLOR seul), un message de refus qui montre les formes
  acceptées, UNDO en une étape. La couleur passe dans l'entité JSON
  (`color`), la transparence dans `geom.transparency` — le .vkd les
  persiste déjà et le STEP writer les transforme en STYLED_ITEM/
  COLOUR_RGB (chemin test_step_color). Testé aussi en bout :
  SETCOLOR -> export STEP -> COLOUR_RGB présent. Doc AGENT.md §2b.
  Tests : tests/test_setcolor.cpp (62 assertions).
- **#4 — import STL : avertissement AVANT l'export STEP.** Un maillage
  qui traverse STEP produit un MANIFOLD_SURFACE_SHAPE_REPRESENTATION
  vide (géométrie perdue, silencieux). `importStl` retourne maintenant
  des `warnings` : le CLI `import` les sort dans un tableau
  `warnings` du JSON, la GUI (File>Open .stl et insertstep) les met
  dans l'historique. Le message nomme le remède (MESH2SOLID) sans
  convertir implicitement — la conversion implicite changerait la
  pièce et resterait plafonnée à 20 000 triangles, le choix explicite
  reste le bon. Doc AGENT.md gotcha STL étendu. Test dédié.

- **Passes :** ctest 448/448 verts (444 + 4 nouveaux) ; gui-smoke
  333/334 — le seul échec (« fresh document empty: got 11 ») est
  l'artefact d'un GUI laissé ouvert par une autre session (socket
  /tmp/vikicad tenu, scénario couru contre lui) ; à relancer après
  fermeture. +1 fix shadow -Wall au passage (OcctViewWidget sk→skId).

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

## 2026-07-23 — Point de départ 3D : sketch sur les plans d'origine ✅

**Le trou UX (rapporté souris par Lex le 17)** : un document 3D VIDE
n'offrait aucun chemin vers un premier sketch — `WORKPLANE` ne
connaissait que XY/OFFSET, aucun menu Sketch, et le clic droit « sketch
sur face » suppose une face existante. « Le point de départ de tout
nouveau projet » était introuvable.

**La solution (commits `f8afa8c` + `be4a69c`)** :
- `WORKPLANE XY|XZ|YZ [OFFSET d]` — repères calculés À LA MAIN (jamais
  via OCCT, cf. le bug des 90° en LESSONS), chacun choisi pour que sa vue
  standard lise le sketch à l'endroit : XY (u=+X, v=+Y, n=+Z, vue TOP),
  XZ (u=+X, v=+Z, n=−Y, face à FRONT), YZ (u=+Y, v=+Z, n=+X, face à
  RIGHT). OFFSET toujours le long de la normale. Rétro-compat stricte
  (`WORKPLANE` nu = XY Z=0, `WORKPLANE OFFSET d` = XY Z=d).
- GUI : menu **Solids ▸ New sketch ▸ Top/Front/Right/Offset plane…** et
  clic droit 3D (sur le VIDE : seul item ; doc plein : après les actions
  existantes) → `beginSketchOnPlane` : WORKPLANE + `SKETCH NEW
  PlaneSketch-N` via le CommandProcessor unique, isolation canvas, ✓
  Finish et retour 3D — le même flux que le sketch-sur-face.

**Clôture de la revue adversariale (verdict CONFORME, 2 minors — les
deux traités aujourd'hui)** :
1. **Piège .vks corrigé** (le vrai bug de la revue) : `WORKPLANE XZ`
   suivi d'une commande sur la ligne suivante avalait son premier token
   (terminaison de l'étape OFFSET optionnelle) puis jetait le reste de
   la ligne — document vide, ok:true. Fix générique `Step::doneRepush()`
   dans le CommandProcessor : le token étranger est re-soumis comme
   nouvelle ligne de commande (sémantique AutoCAD .scr). Reproduit par
   test AVANT le fix ; bonus : le chaînage `WORKPLANE XZ RECT 0,0 10,10`
   sur UNE ligne marche désormais, en script comme dans la barre.
2. **gui-smoke couvre enfin le sketch-sur-face** : phase « face: »
   (box 40x30x10 → viewdir TOP → pick3d centre → sketchface → CIRCLE
   20,15 r5 → SKETCH CLOSE → EXTRUDE 5), cylindre vérifié au 1e-6 :
   bbox (15,10,10)-(25,20,15), volume π·125 = 392.699 mm³.

**Chiffres** : ctest 5142/334 → **5287 assertions / 340 cas** ;
gui-smoke 224 → **260 checks**, ALL GREEN. Reste : validation souris de
Lex (liste en tête de REPRISE.md).

## 2026-07-23 — Ergonomie sketch : préfixes/alias, RECT D, Propriétés rectangle, Extrude au clic droit ✅

**Les 3 manques rapportés souris par Lex (2026-07-23)** :
1. « r/re/rec » ne font rien, seul RECT marche ; il veut entrer les
   dimensions naturellement (« d ») avant ou après le premier clic.
2. Les propriétés d'un rectangle n'exposent pas ses dimensions.
3. Clic droit sur un profil de sketch fermé en 3D : Move ▸ / New
   sketch ▸ seulement — pas d'Extrude.

**La solution (commits `316c433`, `3c852ce`, `3e436ef`, `9698336`
+ clôture)** :

- **Résolution de préfixe unique dans le CommandProcessor** (`resolveName`,
  parité CLI/IPC/GUI gratuite) : exact d'abord (nom OU alias), puis
  préfixe unique dédupliqué par commande CANONIQUE (`REC` matche
  RECT+RECTANGLE = une seule commande → RECT). Ambigu = REFUS listant
  les candidats (`ambiguous command R — matches RECT, REDO, REVOLVE,
  ROTATE, ROTATE3D`) — rien n'est jamais lancé au hasard. La liste parle
  en noms canoniques (`D` liste ERASE via DEL — voulu, cf. LESSONS).
- **Décision alias** : sur dump du registre réel, TOUTE la table AutoCAD
  visée existait déjà (L, C, A, PL, M, CO/CP, RO, MI, SC, TR, O, F, CHA,
  E, X, Z, U, EX→EXTEND, EXT→EXTRUDE…) ; seul **REC→RECT** manquait,
  ajouté. Aucun alias refusé/retiré ; `EX` RESTE l'alias exact d'EXTEND
  (l'exact bat le préfixe), verrouillé par test. L'autocomplétion rend
  les alias en `REC → RECT` ; la barre mappe la ligne vers le canonique
  SANS toucher au traitement d'Enter/popup (leçon historique respectée).
- **RECT Dimensions** (sémantique RECTANG) : `First corner or
  [Dimensions]:` → point OU D ; `Length:`/`Height:` SIGNÉS (négatif =
  l'autre côté), `First corner:` ensuite si absent ; D accepté aussi au
  prompt du 2e coin ; preview live partout. Pas d'étape optionnelle ⇒
  pas de doneRepush nécessaire (documenté dans le code).
- **Propriétés rectangle** (`core/doc/RectProfile.{h,cpp}`) : détection
  = polyligne fermée 4 sommets sans bulge, angles droits (|cos| ≤ 1e-6),
  orientation quelconque. Ancre = sommet le plus proche de l'origine
  monde (égalité → indice le plus bas), Length = grand côté ; l'édition
  étire le long des directions ORIGINALES, ancre fixe. Lignes
  Length/Height éditables dans PropertiesPanel (pattern featureparams),
  TransactionScope, refus des valeurs ≤ 0.
- **Extrude… au clic droit 3D** : sur un profil fermé extrudable (même
  critère `wiresFromEntities` que la commande), item avant Move ▸ ;
  dialogue hauteur mm (retient la dernière), puis `EXTRUDE <h>` via le
  CommandProcessor partagé — tient au prompt Mode <New> avec le ghost,
  Enter commite. AGENT.md §2a documente la parité headless.

**Clôture de la revue adversariale (verdict CONFIRMÉ, 0 critical/major,
3 minors)** : (1) commentaire de test 724→925 corrigé
(test_rect_profile.cpp — assertions déjà justes) ; (2) liste ambiguë en
noms canoniques = design assumé, consigné dans LESSONS ; (3) piège IPC
PRÉEXISTANT (exec laissant un prompt ouvert avale les exec suivants,
déblocage `exec ""`) documenté dans LESSONS, correctif processeur remis
à un lot futur.

**Chiffres** : ctest 5287/340 → **5848 assertions / 348 cas** ;
gui-smoke 260 → **281 checks** (nouvelle phase « ergo: » : REC en IPC,
refus de R, RECT D sommets prédits à la main, lignes Propriétés lues via
query ui, extrusion 6000 mm³ = la ligne même du clic droit, UNDO total).
TOUS VERTS. Reste : validation souris de Lex (liste en tête de
REPRISE.md).

---

## 2026-07-25 — STL en import + couleurs STEP récupérées (XCAF) ✅

**Contexte.** Deux constats de Lex. (1) « vikicad ne prend pas en charge
les STL » : l'export existait depuis la nuit autonome du 09, l'import
n'avait jamais été écrit — un `.stl` tombait dans la branche kit Gerber
et sortait `E_GERBERKIT: no Gerber or Excellon files recognized`, erreur
trompeuse. (2) « on semble perdre les couleurs des STEP lors de la
conversion, c'est le cas ? ou il n'y en a juste pas de sauvegarde dans le
step ? » — question tranchée par la mesure avant d'écrire une ligne.

**Réponse à la question des couleurs, prouvée sur les fichiers de Lex.**
Elles SONT dans les fichiers, et c'est vikiCAD qui les jetait :
`BOUCLIER_BLACK_N_ClearBlue.step` porte 2 `COLOUR_RGB` + 3 `STYLED_ITEM`,
`ELECTRONIK_BLACK_N_WHITE.step` 3 + 13, `RobotSumoS5M0V1.step` 3 + 5 ;
en revanche `ESP32 Heltec WIFI.STEP` (AP203) n'en porte aucune — donc les
deux cas existent et il fallait pouvoir les distinguer. Reproduction
headless avant correction : les 3 solides de BOUCLIER arrivaient tous en
`bylayer`, sans nom ni transparence.

**Trois endroits perdaient la couleur, pas un :**
1. `importStep` lisait avec `STEPControl_Reader` — géométrie SEULEMENT.
2. `exportStep` écrivait avec `STEPControl_Writer` — donc l'aller-retour
   restait destructeur même une fois le lecteur corrigé.
3. `exportObj` n'émettait aucune matière, et côté plugin Obsidian un
   `MeshStandardMaterial` gris unique écrasait tout inconditionnellement.

**Livré.**
- **`importStl`** : `RWStl::ReadFile` (angle de fusion π/2, soudure des
  sommets coïncidents) → `Poly_Triangulation` → `BRep_Builder::MakeFace`
  → UNE face sans surface, logée dans le `SolidEntity` existant. Aucun
  nouveau type d'entité, aucun nouveau renderer : la sérialisation
  BinTools passe `withTriangles=true` (vérifié, sinon un maillage
  disparaîtrait en silence à la réouverture du `.vkd`), et une face
  porteuse d'une seule triangulation n'a AUCUNE `TopoDS_Edge` — donc la
  garde à 4000 arêtes de `updateCache()` saute le HLR d'office, sans quoi
  un maillage de 100k triangles figerait le canevas 2D.
- **`MESH2SOLID`** (alias `M2S`) : triangles → faces planes →
  `BRepBuilderAPI_Sewing` → coque, puis solide si elle est fermée. Refus
  net au-dessus de 20 000 triangles, plafond fixé SUR MESURE et non au
  jugé : 4 014 tri → 0,7 s ; 11 140 → 2,9 s ; 52 484 → 39,3 s et 817 Mo ;
  107 628 → 48,3 s et 1,64 Go (build debug). La mémoire est le vrai mur.
  `VIKICAD_MESH2SOLID_MAX` relève le plafond (même idiome que
  `VIKICAD_STEP_UDA`). Les triangles d'aire nulle sont sautés et comptés
  plutôt que de faire échouer toute la conversion.
- **`importStep` via `STEPCAFControl_Reader` + XCAF** : couleur,
  transparence et nom de pièce par solide. La recherche se fait AU NIVEAU
  DU SOLIDE et sur la forme NON DÉPLACÉE (c'est celle qu'XCAF indexe) :
  SolidWorks écrit `STYLED_ITEM → MANIFOLD_SOLID_BREP`, un par corps, et
  une recherche au niveau pièce ne trouve rien — constaté, puis corrigé.
  Parcours récursif de l'arbre d'assemblage avec composition des
  placements. Nouveaux compteurs `colored`/`named` dans `StepResult` et
  dans le JSON du CLI : ils disent ce que le FICHIER portait, pour que
  « aucune couleur » ne soit jamais confondu avec « couleur perdue ».
- **`exportStep` via `STEPCAFControl_Writer`** : symétrique. Un solide
  `ByLayer` reste NON stylé (ne pas cuire la couleur de calque du jour
  dans le fichier). L'injection des notes AP242 passe désormais par
  `writer.ChangeWriter().WS()->Model()`.
- **`exportObj` + `.mtl`** : une matière par couple couleur+transparence,
  `usemtl` avant les faces, `o <composant>` pour l'outliner. Aucun `.mtl`
  ni `mtllib` quand le modèle est entièrement `ByLayer` — une
  bibliothèque vide ne ferait que déclencher des avertissements.
- **`silenceOcctMessages` factorisé** dans `core/io/OcctMessages.h` :
  vérifié qu'un STL tronqué fait imprimer « premature end of file » par
  OCCT sur **stdout**, ce qui corrompait le JSON du CLI. Les deux cas
  (fichier bidon, fichier tronqué) rendent maintenant du JSON valide.
- **Câblage** : CLI `import IN.stl`, File>Open (+ filtre), et
  `insertStepFile` renommé `insertComponentFile` — il accepte STEP ET STL,
  ce qui est le vrai usage des 13 STL du coffre (pièces du commerce à
  placer dans un assemblage). Le nom de fichier ne sert plus que de repli
  quand le fichier ne porte pas de nom de pièce, sinon il écrasait ce
  qu'XCAF venait de récupérer.

**Leçons.**
- **Un STL binaire dont les 80 octets d'en-tête commencent par le mot
  `solid` est courant** — 12 des 13 fichiers du coffre. Toute détection
  ASCII/binaire par préfixe se trompe ; l'arithmétique
  `taille == 84 + 50 × n` est la bonne (vérifiée à l'octet près sur 4
  fichiers). OCCT et STLLoader font tous deux la bonne chose.
- **Un nom de matériau SolidWorks ne dit pas sa couleur** :
  `COLOUR_RGB('Opaque(232,173,35)', 0., 0., 0.)` — le nom garde les
  valeurs du sélecteur, le fichier écrit du noir pur. Rapporter
  fidèlement le fichier, jamais le nom.
- **Un style STEP peut être orphelin** : dans ELECTRONIK, la chaîne du
  noir s'arrête à un `PRESENTATION_STYLE_ASSIGNMENT` que RIEN ne
  référence. 12 styles → 12 solides colorés, 3 sans style, 1 couleur
  morte : tout est comptabilisé, rien n'est perdu.
- `MESH2SOLID FORCE` a été tenté puis abandonné : à un prompt
  `EntitySet` l'analyseur avale les identifiants en bloc, un mot-clé y
  casse la grammaire (et LESSONS note déjà qu'un prompt laissé ouvert
  avale la commande suivante). Variable d'environnement à la place.
- Le commentaire d'en-tête du plafond affirmait d'abord « 50k prend des
  minutes et des gigaoctets » — écrit AVANT de mesurer. Faux sur le temps
  (39 s), juste sur la mémoire. Remplacé par le tableau des mesures.

**Tests d'injection (règle du dépôt : exiger la preuve, pas le vert).**
`SetColorMode(false)` → 2 cas / 6 assertions tombent. `usemtl` supprimé de
l'export OBJ → 1 cas / 1 assertion tombe. Le premier essai d'injection
n'avait PAS pris (motif `sed` non apparié, 0 occurrence) : le « vert »
observé portait sur du code intact — refait correctement. Fichiers
restaurés, empreintes md5 identiques.

**Chiffres** : ctest 5848/348 → **6049 assertions / 364 cas** ;
gui-smoke 281 → **292 checks** (phase `meshimp:` : export STL binaire de
684 = 84+50×12 octets, réouverture par le dispatch File>Open, bbox du cube,
rendu 3D, MESH2SOLID à 1000 mm³ exactement, UNDO). TOUS VERTS.

**Projet compagnon** (dépôt `vikicad-obsidian-viewer`) : `stepView.ts`
devient `mesh3dView.ts` et sert les deux formats. Un `.stl` est lu
DIRECTEMENT par three.js — aucune conversion, aucun `vikicad-cli` — donc
c'est le seul format 3D qui marche sur toutes les machines de Lex. Le
`.mtl` voisin est chargé quand il existe, et le matériau de repli ne
s'applique plus que là où le fichier ne dit rien. Vérifié hors navigateur
sur les vrais fichiers : les 3 solides de BOUCLIER arrivent avec
`vikicad_0` noir et `vikicad_1` gris `#a0a0a0`, et le DHT22 (en-tête
`solid`, binaire) donne 11 140 triangles et 15,0 × 10,8 × 40,6 mm.

**Reste à faire** : validation souris par Lex (tour en tête de
REPRISE.md). Le « ClearBlue » du nom de BOUCLIER n'a jamais atteint le
STEP — réglage d'export SolidWorks, hors de notre portée.

## 2026-07-25 — Presse-papier : Ctrl+C/X/V, y compris entre documents ✅

**Demande de Lex** : « le ctl-c / ctl-v doit fonctionner… et entre les
documents aussi. »

**Livré.** Trois commandes nouvelles dans le processeur partagé —
`COPYCLIP`, `CUTCLIP`, `PASTECLIP` — donc identiques au clavier, au menu
Édition (Couper/Copier/Coller), en CLI, en script et en IPC. Le payload
(`core/io/ClipboardIo.h`) est un JSON AUTONOME sous le type MIME
`application/x-vikicad-entities` : les entités (sérialisation
toJson/entityFromJson déjà éprouvée par le .vkd et l'undo — le BREP des
solides voyage en base64) PLUS tout ce qu'elles référencent par nom ou
par id : calques, définitions de blocs (récursif pour les inserts
imbriqués), styles de cote. Au collage, remappage par NOM : un calque
manquant est créé avec ses propriétés transportées ; un calque existant
du même nom GAGNE tel quel (ses couleurs appartiennent au document
cible). Nouveaux ids, une seule transaction, le collage devient la
sélection courante (prêt pour un MOVE immédiat).

**Pourquoi ça marche entre les documents** : vikiCAD est mono-document
par processus, donc « entre les documents » = entre processus = le
presse-papier de l'OS. La GUI branche un `ClipboardHook` (QClipboard +
QMimeData) sur le `CommandContext` ; sans crochet (CLI headless, tests),
un tampon local au processus prend le relais — copier/coller reste
fonctionnel dans un script.

**Interaction.** Ctrl+V demande le point d'insertion (l'ancre = coin
inférieur gauche de l'ensemble copié), ghost 2D sous le curseur et ghost
3D bleu (compound des solides) dans la vue OCCT ; **Enter colle À LA
POSITION D'ORIGINE** — le cas d'alignement entre deux documents. En
headless strict, le Finish implicite donne le collage sur place. Les
xlines sont exclues du calcul d'ancre (leur boîte sentinelle l'aurait
projetée à ±1e9).

**Qt fait le tri texte/entités tout seul** : les QActions en
`ApplicationShortcut` perdent volontairement contre un QLineEdit focusé
via ShortcutOverride — taper dans la barre de commande copie/colle du
TEXTE, exactement comme Ctrl+Z y fait déjà l'undo de texte. Aucun code
de garde à écrire.

**Leçons.**
- **PASTECLIP sélectionne son collage, donc la commande TAPÉE suivante
  hérite du pickfirst** : au smoke, `CUTCLIP <ids>` a mangé la sélection
  et avalé les ids en silence — 1 entité restait. Même piège que la
  phase ergo, même remède (`SELECT` nu d'abord). C'est le comportement
  voulu en GUI (Ctrl+X coupe la sélection), mais en script : vider la
  sélection avant toute commande à ids explicites.
- **Le presse-papier même-processus ne prouve RIEN du transport OS** :
  Qt ressert son propre QMimeData sans que les octets traversent le
  compositeur. D'où la phase `xproc:` : DEUX instances réelles, A copie,
  B (qui VOLE la socket IPC `vikicad` — `removeServer` + `listen`)
  colle. Vérifié sous Wayland : « 1 object(s) pasted », bornes exactes.
  La phase est en FIN de smoke (la socket volée rend A muet) et tolère
  un SKIP si le compositeur refuse le clipboard à une fenêtre sans focus.

**Tests d'injection (la preuve, pas le vert)** : remap de calques
supprimé → le cas inter-documents tombe ; ancre = centre au lieu du coin
→ 2 cas / 3 assertions tombent. Fichier restauré, md5 identique.

Le prompt d'insertion suit la règle des jalons optionnels (AGENT.md §2.6) :
un token étranger prend le défaut (collage sur place) et REPOUSSE le token
vers une nouvelle ligne de commande — `PASTECLIP` puis `RECT …` en .vks ou
en IPC exécute les deux au lieu d'avaler le RECT (le piège .vks
historique, testé).

**Chiffres** : ctest 6049/364 → **6155 assertions / 373 cas** (9 cas
presse-papier : même document, entre documents, calques remappés, solide
avec BREP+couleur+transparence+composant, CUTCLIP+UNDO, payload étranger
refusé sans toucher au document, token étranger au prompt d'insertion,
bloc transporté, style de cote transporté) ; gui-smoke 292 →
**310 checks** (phase `clip:` 16 lignes + postlude `xproc:` 2 lignes).
TOUS VERTS.

## 2026-07-26 — Vue DWG du plugin : les faux triangles de hachure ✅

**Constat de Lex** : sur `Pyramide.dwg`, la vue Obsidian noyait le plan du
2e étage sous de grands triangles bleus hachurés ; vikiCAD, lui, rendait
bien. **Deux bogues de dxf-viewer 1.0.48**, tous deux corrigés DANS le
plugin (`src/hatchBoundary.ts`), zéro modification vendorée.

**L'enquête, parce que la piste courte mentait.** Les hachures du fichier
sont saines (l'inventaire DXF le prouvait), la scène headless semblait
saine aussi — parce que je mesurais l'ÉTENDUE des lots, pas leur contenu.
La percée : sonder les PIXELS dans le banc navigateur (`verify/dxf.html`),
puis masquer les objets three.js un à un jusqu'à perdre le bleu. Coupable :
le lot de lignes du calque `2e_wall_ruff` — le « triangle plein » était en
fait des MILLIERS de segments ANSI31 remplissant une région fausse. Le
tracé SVG de la frontière a montré le zigzag : des diagonales parasites
d'un coin à l'autre du plan.

**Bogue 1 — arêtes en vrac.** Une frontière de HATCH « liste d'arêtes »
ne garantit ni l'ordre ni l'orientation (AutoCAD ré-enchaîne à la lecture ;
il écrit d'ailleurs le MÊME contour dans deux ordres différents pour les
deux hachures jumelles du bloc `w2b`). dxf-viewer concatène tel quel :
première arête inversée = polygone en zigzag = auto-intersections = la
parité remplit des nœuds papillon triangulaires. Correctif : tesseller
chaque arête séparément (les routines de dxf-viewer restent maîtresses),
puis reconstruire le contour par CIRCUIT EULÉRIEN (Hierholzer) — un
glouton « prochaine arête qui touche » se coince aux jonctions des PONTS
(le contour sort vers un îlot et en revient). GARDE-BARRIÈRE : si chaque
arête suit déjà la précédente, le code d'origine fait foi à l'identique —
no-op strict, vérifié au segment près.

**Bogue 2 — boucles perdues après 97/330.** ParseEdge avale le code 97
(compte de références d'associativité) qui clôt chaque boucle ; le 330
orphelin fait rendre null à ParseBoundaryLoop et TOUTES les boucles
suivantes de la hachure sont perdues — ici la boucle intérieure
(« outermost »), donc la parité n'avait plus que l'externe et remplissait
bien trop. Correctif : purge textuelle des blocs 97/330 des HATCH avant le
parseur (même famille de remède que le recollage LibreDWG), ces références
n'ayant aucune valeur pour un rendu.

**Vérification** (`verify/verify-hatch.mjs`, recensement par entité
AVANT/APRÈS) : reproduction avérée (1209/1211 segments sur 929×929) ;
encre dégonflée 88 249 → 38 807 (la bande de mur exacte) ; NO-OP STRICT
sur les hachures réellement ordonnées (NET, ANSI31 fine, AR-CONC et ses
6 069 segments — identiques au segment près) ; les inondations DISCRÈTES
des autres hachures de murs réparées au passage (70A5 : 1 577 → 492
d'encre — brouillées pareil, juste moins visibles) ; plein fichier de
48 Mo re-vérifié in situ. Banc navigateur A/B au pixel : pièce inondée
[50,109,202] → fond [38,38,42], encre 9,20 % → 6,64 %.

**Leçons.**
- **Mesurer l'étendue d'un lot ne dit rien de son contenu** : l'inondation
  vivait dans un lot à l'emprise parfaitement normale. C'est la sonde de
  pixels + la bissection de visibilité qui ont nommé le coupable, pas
  trois généreuses passes de statistiques géométriques.
- **Mes « hachures saines » de contrôle ne l'étaient pas** : mêmes arêtes
  brouillées, inondation discrète. Un témoin de non-régression se PROUVE
  (ordre des arêtes vérifié), il ne se devine pas à l'œil.
- **Un harnais async qui restaure son crochet avant l'await mesure du
  vide** : premier « TOUT VERT » à zéro checks. Depuis : le harnais compte
  ses propres checks et échoue sous 8.
- Le cache navigateur heuristique a servi un bundle périmé pendant la
  vérification (python http.server n'envoie aucun Cache-Control) — marqueur
  global + nom de fichier neuf pour trancher, puis retour au nom canonique.

**Portée** : le correctif vit CÔTÉ VISIONNEUSE, donc il s'applique quel
que soit le convertisseur amont (dwg2dxf primaire ou secours vikiCAD) et
à tout `.dxf` ouvert tel quel. Le cache de conversions n'a pas à être
invalidé : il contient le DXF d'entrée, pas le rendu.

## 2026-07-26 — Enregistrer sous multi-formats, export DWG, VKD dans Obsidian ✅

**Demande de Lex** : « quand je sauvegarde, je ne peux pas choisir le
type… seulement en VKD (et export devrait être aussi intégré au "save
as") ; dans Obsidian un fichier VKD ne s'affiche pas en preview, il
s'ouvre directement dans vikiCAD ! (oups) ; je ne peux pas exporter en
DWG ? »

**Livré.**
- **« Enregistrer sous » parle tous les formats** : VKD, STEP, DXF, DWG,
  STL, OBJ dans le même dialogue. Un format non natif EXPORTE (même moteur
  que File ▸ Export) sans toucher au document courant — le `.vkd` reste le
  fichier de travail, aucun format avec pertes ne devient silencieusement
  le document. Sans suffixe tapé, le filtre choisi décide du type.
- **Export DWG** (une première) : `core/io/DwgExporter` écrit un DXF r2000
  temporaire puis le convertit par `dxf2dwg` (GNU LibreDWG, résolu PATH
  puis `~/.local/bin`, comme l'import). Câblé partout : menu Export,
  Enregistrer sous, IPC `export`, CLI `export FILE.vkd OUT.dwg`. Le succès
  se juge au FICHIER produit (converti dans le dossier temporaire, copié
  sur la cible seulement s'il existe et pèse plus de zéro octet).
- **Deux patches libdxfrw** pour écrire un DXF canonique que LibreDWG
  digère :
  - **0005** — owner `330` sur chaque entité (`1F` en model space, le
    record du bloc pour son contenu) et plus de section CLASSES vide.
    Sans owners, dxf2dwg rangeait les entités ORPHELINES dans la carte
    d'objets : un DWG plein… que tout lecteur relisait VIDE.
  - **0006** — rotation MTEXT en vecteur de direction `11/21/31` (ce
    qu'AutoCAD écrit) au lieu du groupe `50`, FATAL pour LibreDWG ; en
    prime notre propre réimport ne devine plus radians contre degrés,
    il lit le vecteur.
- **VKD s'affiche DANS Obsidian** (dépôt vikicad-obsidian-viewer) :
  extension `vkd` enregistrée, vue d'aiguillage `VkdView` — un
  `query --describe` (aucune conversion) décide : au moins un solide →
  vue 3D (export OBJ+MTL), sinon vue 2D (export DXF, mêmes traits fins
  que les DWG) ; branches `vkd` dans DwgView et Mesh3dView, conversions
  au même cache que le reste. Avant : cliquer un `.vkd` ÉJECTAIT hors
  d'Obsidian vers vikiCAD.

**Preuves.** ctest **375/375** (2 cas DWG neufs : géométrie + hachure +
calque via `importDwg` — l'autre implémentation — et blocs+INSERT
préservés) ; gui-smoke **313 TOUT VERT × 3 passes** (nouvelle phase :
export DWG par IPC, DWG non vide, relecture dwg2dxf exigeant des
entités) ; échelle réelle : Pyramide.dwg importée (5 186 entités) →
export DWG 1,7 Mo en ~43 s → relecture indépendante **5 186/5 186**
(3 759 LINE, 392 INSERT, 386 ARC, 316 LWPOLYLINE, 200 CIRCLE, 88 TEXT,
30 ELLIPSE, 10 HATCH, 3 MTEXT, 2 SPLINE) et **13/13 hachures de blocs**
présentes dans les définitions relues. Plugin : build propre, aiguillage
vérifié headless (0 solide → 2D ; 1 solide → 3D ; exports produits).

**Leçons** (détail dans LESSONS.md) : un écrivain de fichier se juge à la
RELECTURE INDÉPENDANTE ; « AutoCAD le lit » ne veut pas dire canonique ;
le message d'erreur d'un parseur peut nommer le mauvais coupable.

**Restes assumés.** LibreDWG émet 6 « Unknown DXF code 21 for HATCH »
NON FATALS sur 2 hachures de blocs de Pyramide (séquences de codes
identiques aux hachures acceptées, seules les valeurs diffèrent — bogue
de parseur LibreDWG, pas de notre écrivain) ; les 13 hachures survivent
au comptage. À cette échelle, dxf2dwg prend ~40 s : c'est le
convertisseur qui travaille, pas nous — le délai de DwgExporter est à
120 s.

## 2026-07-26 — Garde de fermeture : plus de travail perdu sans avertir ✅

**Demande de Lex** : « et si je ferme vikiCAD sans avoir sauvegardé, j'ai
un avertissement offrant la sauvegarde ? » — la réponse était NON : aucun
`closeEvent`, aucun suivi de modification ; fermer, New, Open et les
imports jetaient le travail en silence, sans même un `*` au titre.

**Livré.** Suivi par IDENTITÉ D'ÉTAT dans le journal d'undo : chaque
transaction commise reçoit un `stateId` unique à vie ;
`Document::stateId()` = l'id de la dernière transaction appliquée (0 =
tel que chargé). « Modifié » = comparaison avec l'id mémorisé à
l'ouverture/sauvegarde — donc Ctrl+Z jusqu'à l'état sauvegardé REDONNE un
document propre, une branche redo abandonnée ne peut JAMAIS repasser pour
sauvegardée (ids retirés à jamais), et l'éviction du plafond d'undo rend
honnêtement l'état « modifié pour toujours ». Câblage GUI :
Sauvegarder / Ignorer / Annuler à la fermeture ET avant New/Open/imports,
`*` au titre rafraîchi par l'écouteur de changements du document, imports
marqués « jamais sauvegardés » (ils n'existent nulle part en .vkd).
L'IPC `open`/`save` ne demande RIEN (un modal gèlerait la socket) ; arrêt
d'unité systemd = SIGTERM, sans closeEvent — consigné dans AGENT.md.

**Preuves.** ctest **376/376** (nouveau cas : ids uniques, undo/redo
retombent sur les MÊMES ids, retour à 0, branche morte jamais réutilisée,
commit vide sans effet) ; gui-smoke **313 TOUT VERT** — preuve en creux
qu'aucun chemin headless ne déclenche le dialogue, sinon le harnais
gèlerait. Validation souris du dialogue au tour de REPRISE.md.

## 2026-07-26 — EAGLE 6+ en import : les vieux projets redeviennent lisibles ✅

**Demande de Lex** : « compatibilité vikiCAD / plug-in Obsidian des vieux
fichiers eagles, pour au minimum voir mes schematic et layout des vieux
projets (édition pas vraiment nécessaire) ».

**Inventaire d'abord.** 234 fichiers .brd/.sch/.lbr dans le LSB : 216 en
XML EAGLE 6.0→8.2.2 (TOUS ses projets perso et jobs), 13 binaires EAGLE ≤5
(uniquement des ressources fournisseurs : Tag-Connect, SparkFun), 5 .sch
TecnoVE d'un AUTRE outil (magie `xV1`, non identifié). Verdict : un
importeur XML natif couvre tout ce qui compte ; pas de parseur binaire.

**Livré.** `core/io/EagleImporter` (QXmlStreamReader, zéro dépendance
neuve) : planches APLATIES (packages cuits à la position des éléments,
miroir → bascule sur les calques b*), pistes = polylignes à largeur réelle
bouts ronds, pads/vias = hachures pleines percées du foret, polygones =
leur contour (EAGLE ne stocke jamais le remplissage calculé), textes
`>NAME`/`>VALUE` substitués (attributs smashed compris), cotes EAGLE →
DimensionEntity/LeaderEntity vivantes ; schémas : résolution
part→deviceset→gate→symbol, broches avec noms, nets avec jonctions et
étiquettes, feuilles multiples posées CÔTE À CÔTE (25 mm). Calques aux
noms/couleurs/visibilités d'EAGLE, ordre de peinture façon EAGLE (cuivre
bottom d'abord, annotations dessus). Câblé partout : File>Open,
File>Import EAGLE, verbe IPC `open`, `vikicad-cli import`, garde de
fermeture et « jamais sauvegardé » comme les autres imports. Refus nets
qui NOMMENT le remède : binaire ≤5 (« resauver en XML dans EAGLE 6+ »),
.lbr, .sch d'un autre outil (sniff de contenu). Plugin Obsidian :
.brd/.sch → import vikiCAD → export DXF (cache eagle2dxf:) → la même vue
dxf-viewer que DWG/VKD, bouton Reconvertir compris.

**Preuves.** ctest **380/380** (142 assertions eagle : synthétiques brd/sch
+ REV3.brd/REV3.sch réels) ; gui-smoke **322 TOUT VERT** (phase eagle : la
planche s'ouvre par IPC, miroir prouvé par l'existence même des calques
Bottom/bNames, binaire refusé en réponse d'erreur SANS modal) ; **balayage
des 145 fichiers réels du vault : 145/145 importent, 0 nœud sauté** (67
planches, 78 schémas dont 15 multi-feuilles jusqu'à 5) ; rendu visuel
contrôlé sur REV3 (planche ET schéma via export PDF : pistes, pads,
sérigraphie, PIC24 broche par broche, ULN2803A transistor par transistor).

**Leçon.** `signals` est un mot-macro Qt : impossible de nommer une
variable ainsi dans du code lié à QtCore sans QT_NO_KEYWORDS. Renommé
`sigs` — l'erreur de compilation (« expected unqualified-id before
public ») pointe la LIGNE mais pas la cause.

## 2026-07-26 — Trio vert : gerbers P-CAD/Protel, IGES, bibliothèques EAGLE ✅

**Demande de Lex** : après l'inventaire des vieux projets (« identifie les
types de fichier qu'on pourrait ajouter »), feu vert sur le trio à meilleur
rendement : extensions Gerber manquantes, import IGES, .lbr EAGLE.

**Livré (1) — les kits P-CAD/Protel des vieilles jobs s'ouvrent.** Le
chantier « extensions » a déterré deux VRAIS trous de parseur : les macros
d'ouverture à variables (`%AMOC8*5,1,8,0,0,1.08239X$1,22.5*` — l'octogone
P-CAD classique) et le `D01` sans `G01` préalable (linéaire implicite
depuis RS-274D). Macros désormais LIÉES au `%ADD` : évaluateur
d'expressions (`$n`, + - x /, parenthèses, moins unaire), copie privée
purement numérique `NOM_Dnn` — anneaux, inspecteur, camMeta et writer ne
voient jamais une variable. Rôles de calques pour les noms courts Protel
(top/bot, smt/smb, sst/ssb, spt/spb). Kit Aki3 témoin : 1 fichier lu avant,
7/7 après (39 130 entités). Côté plugin : gm13/15/16, gpt/gpb, tap et les
noms Protel enregistrés, avec sniff de contenu pour les extensions
génériques avant d'entrer dans un kit.

**Livré (2) — IGES en import.** Même pipeline XCAF que STEP
(IGESCAFControl_Reader, TKDEIGES au lien) ; les IGES du monde réel sont des
surfaces découpées, pas des solides — le repli « fichier sans solide = UNE
entité affichable » est le chemin normal. Chaîne plugin : .igs → OBJ (la
conversion STEP était déjà générique). Testé sur les modèles fournisseurs
de C1P0 (DHT22, ESP32).

**Livré (3) — .lbr EAGLE en planche-contact.** Chaque package et symbole =
une cellule d'une grille en rangées, nom dessous sur un calque « Labels »
dédié, >NAME/>VALUE laissés littéraux comme dans l'éditeur de bibliothèques
d'EAGLE. Balayage réel : **73/73 bibliothèques XML du vault, 0 nœud
sauté**, jusqu'à 495 679 entités (TI element14). Rendu contrôlé sur
ALRMP_RLC (grille de centaines d'empreintes étiquetées).

**Preuves.** ctest **388/388** (5 cas macros/G01, 3 cas IGES dont un vrai
fichier, 2 cas .lbr dont ALRMP_RLC réel) ; gui-smoke **325 TOUT VERT**
(phase eagle étendue au .lbr par IPC) ; kits Aki3 rendus par gerbv
(la route du plugin) et vérifiés à l'œil.

**Leçon.** Un chantier « liste d'extensions » peut cacher un trou de
PARSEUR : vérifier le moteur sur un vrai fichier AVANT de conclure que
seule la liste manque. Les deux archaïsmes étaient invisibles tant que le
sniff d'extension écartait les fichiers.

## 2026-07-26 (soir) : chantiers jaunes, SchLib en planche-contact et Visio dans Obsidian

**Livré (1) : .SchLib Altium en planche-contact (plugin).** Nouveau
`tools/python-altium/schlibgrid.py` (fichier à nous, à côté de la copie
vendorisée) : lit la structure propre aux bibliothèques (un storage OLE
par composant, stream Data) et réutilise les handlers de dessin
d'altium.py. Le morceau dur : les broches y sont des records BINAIRES,
pas du texte comme dans les SchDoc. Layout établi par sonde empirique et
validé sur les **5778 broches du vault (0 échec)**, documenté en tête du
script. Une cellule par composant ET par partie (« (2/4) » pour un quad
op-amp), nom en gras + description en italique dessous, titres à
l'échelle des cellules. Balayage réel : **257/257 .SchLib du vault
rendus sans erreur**, contrôle visuel sur LM358 (SnapEDA), PIC24
(import PADS), SS.SchLib (55 composants TecnoVE) et la bibliothèque PDM
(multi-parties). Les .PcbLib restent exclus : binaires, aucun lecteur
libre.

**Livré (2) : Visio .vsd et métafichiers .emf/.wmf (plugin).** Chaîne
LibreOffice headless (profil dédié dans le cache, conversions mises en
FILE car LibreOffice n'accepte qu'une instance par profil) vers un PDF
de travail, puis pdftocairo découpe une page SVG par page ; vue SVG
partagée avec boutons de pages ◀ ▶ (instantanés, cache par page).
Balayage réel : **90/90 fichiers du vault convertis** (44 .vsd + 46
.emf/.wmf, 16 multi-pages jusqu'à 8 pages). Contrôles visuels : GRAFCET
PIN1004 (4 pages, SFC complet lisible) et gabarit Eurocard VME 6U.
Réglages : chemins soffice et pdftocairo (pdfinfo cherché en voisin).

**Preuves.** Sondes empiriques avant le code (5778 broches, sémantique
PARTCOUNT = max(1, PARTCOUNT-1, max OWNERPARTID) vérifiée sur les deux
conventions d'écriture) ; build TypeScript propre ; corpus complets
257/257 et 90/90 ; PNG de contrôle via LibreOffice (Firefox headless
gèle sur cette machine, bogue snap connu).

**Leçon.** Sur un format binaire non documenté, l'ordre des champs se
prouve par les VALEURS LÉGALES : l'ordre inner/outer des symboles de
broche n'était juste que dans un sens (clock=3 d'un bord, bulle=1 de
l'autre) et le corpus entier fait office de test. Sonder d'abord, coder
ensuite : les trois sondes ont coûté dix minutes et ont éliminé toute
hypothèse fausse avant la première ligne du renderer.

## 2026-08-03 : vue 3D du plugin en Z-vers-le-haut

**Livré.** Rapport de Lex : « le clic-glisse ne fonctionne pas toujours
dans tous les sens » dans la vue 3D d'Obsidian, tous formats. Cause : les
modèles CAO (STEP, STL, VKD, IGES) sont Z-vers-le-haut, mais la vue
orbitait autour de l'axe Y (convention three.js) — le drag horizontal
faisait culbuter la pièce au lieu de la faire pivoter, et près des pôles
le drag vertical mourait d'un côté. Vérifié sur le cache réel : 4 des 6
OBJ convertis sont des plaques posées dans le plan XY. Correctif :
caméra en up=Z, point de vue de départ isométrique, éclairage réaligné —
mêmes gestes que la vue 3D de vikiCAD (OCCT). Commit 726a36c côté
plugin, tour souris validé.

**Preuves.** Banc Node sans WebGL : vrais événements pointeur simulés
sur OrbitControls (stub d'élément avec getRootNode, r185 écoute
pointermove sur la racine). Avant : un drag horizontal change l'altitude
de la pièce de 12,8 unités (culbute). Après : altitude invariante à
1e-14 près, drag vertical incline normalement. 4/4.

**Leçon.** Un vecteur de test symétrique peut prouver n'importe quoi :
avec un offset x=z et un drag d'exactement 90 degrés, dz restait nul
MÊME sur le mauvais axe — le premier banc était vert à tort d'un bord.
Asymétriser les données de test (offset quelconque, angle non
remarquable) avant de croire un invariant géométrique.

## 2026-08-05 — Module d'animation : chaînes cinématiques, GLB, WebP, CLI, timeline ✅

**Demande de Lex.** Chantier intensif : un module d'animation GÉNÉRIQUE
dans vikiCAD pour le pont GenMov3D (Moment_MYP → avatar 3D animé), au
contrat d'interface `../GenMov3D/docs/CONTRAT-VIKICAD.md`. Exigence
cadre : l'humanoïde n'est qu'une chaîne prédéfinie — le cœur doit servir
autant les mécanismes techniques (pivot, glissière, vues éclatées) que le
yoga. Ordre impératif : CLI d'abord (GenMov3D bloqué dessus), timeline
GUI ensuite.

**Livré (1) — core/anim.** Chaînes typées ball/revolute/prismatic/fixed/
free (tri topologique, fractions de scale_reference → mm, butées par
canal en avertissements jamais en clamp), pose3d v1 densifié par report
de canaux, slerp quaternion (Euler extrinsèque XYZ), cinématique directe
composée comme les placements d'assemblage STEP, racines TYPÉES composées
avec root_pos/root_rot (un levier mono-pivot s'anime). Avatar v1 derrière
l'interface AvatarProvider (couture du futur avatar skinné) ; mannequin
rigide = capsules OCCT fusionnées + tête ellipsoïde + styles
d'articulation. Écrivain clipToJson pour le réexport.

**Livré (2) — sorties.** GLB binaire écrit à la main (QJson + conteneur),
nœud de conversion Z-up mm → glTF Y-up mètres FACE +Z, canaux quaternion
sign-alignés, pingpong baké sur la fenêtre de boucle ; relu par un
lecteur INDÉPENDANT (cgltf 1.15 vendorisé, tests seulement). WebP animé
natif (libwebpmux en flux, jamais ~300 Mo de trames en mémoire), modes
pingpong/cycle/hold. Renderer offscreen dans une lib séparée
(vikioffscreen — la visu reste hors de core) : fenêtre virtuelle,
buffersOpaqueAlpha=false SONDÉ AVANT d'écrire la classe → fond alpha 0
sans matting ; caméra orthographique FIXE cadrée sur l'union de toutes
les trames, sol z=0 en bas (précision contrat du jour même : side par
défaut). CLI `anim render` conforme : erreurs stderr ET JSON stdout,
garde anti-traversée par le pattern d'id, plafond 300 s.

**Livré (3) — timeline GUI.** Dock Timeline (patron AssemblyPanel) :
chargement pose3d + avatar, sélecteur d'avatars, lecture/scrub avec
marqueurs de keyframes cliquables, édition légère (t d'un keyframe,
canaux d'une articulation), réexport ; overlay d'animation dans
OcctViewWidget posé par SetLocalTransformation (premier usage du dépôt —
jamais de rebuild de scène) ; verbe IPC `anim` pour le pilotage headless.

**Preuves.** Suite 388 → 430 cas verts ; 15/15 poses pilotes réelles
rendues (240 trames chacune, ~4,5 s/pose, WebP 0,4–1,2 Mo) ; GLB validé
par cgltf_validate + déterminisme octet (GLB, WebP, trames) ;
vérification VISUELLE des trames (l'Arbre, savasana couché, chien tête
en bas) ; gui-smoke 334 checks dont scrub-retour exact au pixel ; CI
GitHub verte (libwebp-dev ajouté, tests de rendu SKIP proprement sans
écran). Signals CLI-PRET et GUI-TIMELINE-PRET posés dans le PROGRESS de
GenMov3D ; deux points à trancher documentés au contrat (résolution du
fichier chain, interprétation du breath).

**Leçons.** (1) Le rendu peut être vert à tous les tests et FAUX à
l'œil : le mannequin est sorti tête en bas (Image_PixMap::Row() compte
déjà du HAUT — ma « correction » bottom-up l'inversait) puis dos à la
caméra dans le GLB (Rx(−90) seul met up en Y mais la face en −Z ; il
faut Ry(180)·Rx(−90) pour la convention glTF face +Z). REGARDER une
trame et vérifier l'orientation par un invariant (pieds en bas) font
partie des tests maintenant. (2) La revue adversariale AVANT le signal
a payé : 18 constats confirmés dont la racine typée gelée — le cas
levier/bras robot exigé par le contrat ne marchait pas alors que les 15
poses humanoïdes passaient toutes. Les données de production ne couvrent
jamais la généralisation promise. (3) Spec vivante : les limites
anatomiques et la caméra du contrat ont changé PENDANT le chantier —
relire les sources amont avant chaque étape, pas seulement au début.

## 2026-08-05 (bis) — chantier esthétique + mains : mannequin sculpté, éclairage studio, humanoid-14

**Demande.** VikiCAD gagne un volet artistique : remplacer le mannequin
« saucisses raccordées » par un mannequin galbé digne d'une app de
bien-être (torse lofté, congés aux articulations, membres fuselés,
matériau doux, éclairage qui module, ombre au sol), produire 3 variantes
sélectionnables + planche comparative, et ajouter le segment main
(chaîne humanoid-14, ~0,09 m, butées réalistes) sans toucher
humanoid-12, aux schémas ni aux pose3d existants. Interface CLI du
contrat inchangée, déterminisme octet, < 10 s/posture.

**Livré (1) — build « sculpted » (core/anim/Avatar).** Champs
d'extension tolérés v1 (proposés pour v2 au contrat,
SCHEMA-AVATAR-V2-DEMANDE) : membres = tubes de révolution fuselés
(profil B-spline approximé degré 3) FERMÉS par calottes hémisphériques
en compound — le rayon distal suit l'enfant, un genou plié à 150° reste
un volume continu (rotule sphérique au pivot en renfort) ; torse = loft
d'ellipses sur profil lissé (48 sections réglées ; épaules bombées +
calottes deltoïdes aux attaches de bras, taille, ourlet rentré dans le
blob de bassin) ; taper/flatten par articulation (pieds, mains).

**Livré (2) — rendu (offscreen).** Rig 3 lumières relatif à la caméra +
shading Phong par pixel (le PBR d'OCCT sans IBL aplatit et rend la
transparence opaque — sondé avant de coder), matériau dérivé de
roughness/metallic, ombre de contact translucide ancrée sous les APPUIS
de l'animation (presentation.ground_shadow), cadrage AddOptimal (les
enveloppes de points de contrôle B-spline gonflaient l'unionBox : figure
minuscule poussée hors cadre).

**Livré (3) — mains (humanoid-14).** Chaîne 14 = 12 segments identiques
+ hand_l/hand_r (~0,09 m, flexion palmaire 80°/extension 70°/déviation
±30°) ; avatar mitaines aplaties ; rétrocompat par accepts_pose_chains
(champ d'extension de chaîne) : le banc humanoid-12 se rend tel quel sur
la 14, mains au repos, avertissement partout (JSON CLI + réponse IPC) ;
clipToJson déclare la chaîne d'ÉDITION (un clip 12 édité sur la 14 porte
des canaux de poignets que la 12 ne peut pas charger).

**Livré (4) — variantes et jugement.** 3 avatars dans GenMov3D
(sculpte sauge / athletique argile rosé / doux bleu-gris) + planche 4×5
(docs/planches/planche-avatars-2026-08-05.png) ; panel de 3 juges à
lentilles distinctes (lisibilité / bien-être / flexion) : manikin-sculpte
retenu 24/30, et ses constats concrets (embouts en cratère, épaulettes,
ombre au centroïde) tous corrigés avant signal.

**Preuves.** Suite 442 → 444 cas verts ; gui-smoke 334 ; 15/15 poses du
banc sans avertissement, ~2,4 s/posture (240 trames), GLB max 1,24 Mo
(47,6 k triangles, plafond 1,5 Mo tenu), déterminisme octet ; revue
adversariale 6 finders/réfuteurs : 6 confirmés corrigés (collerette
rasoir du loft sur joint court générique, deltoïdes codés ±Z, replis
silencieux de parsing, torse longueur nulle muet, avertissements absents
de la réponse IPC), 5 réfutés avec preuves ; signaux AVATAR-BEAU-PRET et
HAND-PRET posés.

**Leçons.** (1) Les lofts B-spline ont DEUX pathologies opposées : le
lisse oscille (taille de guêpe, jupe, tente explosée), le réglé facette ;
la recette stable = profil APPROXIMÉ bas degré puis loft réglé dense
échantillonné dessus — et borner les DEUX axes de l'échantillon, pas
seulement Z. (2) Un pôle de révolution sur profil approximé se replie en
cratère : ne jamais fermer un solide par le pôle du profil, fermer par
une calotte sphérique explicite. (3) Le panel de juges esthétiques à
lentilles distinctes voit ce que l'auteur ne voit plus (l'ombre au
centroïde « faisait flotter » le mannequin — évident une fois dit) ; la
revue adversariale ensuite attrape ce que le panel ne cherche pas (la
généralisation aux mécanismes, encore elle). (4) Piège mesuré : comparer
des largeurs de silhouette en comptant les pixels opaques d'une RANGÉE
entière mélange bras et torse — mesurer la bande contiguë centrale.
