# VikiCAD

**[🇫🇷 Français](#français)** | **[🇬🇧 English](#english)**

---

## Français

CAO 2D (dessin technique) + solides 3D + édition de fichiers de fabrication
PCB (Gerber/Excellon), pour Linux. Née comme remplacement personnel de
nanoCAD, pensée dès le départ pour être pilotable par des agents IA aussi
bien qu'à la souris.

- **Stack** : C++17, Qt6 Widgets, OpenCASCADE 7.9, format natif SQLite
  (`.vkd`), CMake.
- **Interop** : DXF R12–2018 (libdxfrw vendoré et patché), DWG en import
  (via `dwg2dxf`/LibreDWG), STEP (OCCT, notes en sidecar JSON ou attributs
  AP242 optionnels), STL/OBJ, PDF, et **Gerber RS-274X + Excellon** en
  lecture/écriture.
- **Pilotable par agent, par conception** : `vikicad-cli` headless en JSON,
  socket JSON-RPC sur la GUI en cours d'exécution (`vikicad-cli connect`),
  scripts `.vks` (sémantique `.scr` d'AutoCAD). Voir
  [docs/AGENT.md](docs/AGENT.md).

### Fonctionnalités

**Dessin 2D** — LINE/CIRCLE/ARC/RECT/PLINE/ELLIPSE/SPLINE/POINT/XLINE ;
MOVE/COPY/ROTATE/MIRROR/SCALE/STRETCH ; TRIM/EXTEND/OFFSET/FILLET/CHAMFER/
BREAK/JOIN/EXPLODE ; accrochages aux objets (extrémité/milieu/centre/
quadrant/intersection/perpendiculaire) + ORTHO/POLAIRE/GRILLE ; poignées de
sommets ; calques ; bascule mm/pouce en un clic (`x,y`, `@dx,dy`,
`@dist<angle`, `2"`, `10mm`).

**Annotation** — MTEXT, 5 types de cotes régénérées en direct depuis le
DimStyle et les unités d'affichage, lignes de rappel, hachures
(SOLID/ANSI31/ANSI37), MATCHPROP.

**Organisation** — blocs avec attributs, réseaux associatifs rectangulaires/
polaires (éditables via ARRAYEDIT), **sticky notes** (markdown, auteur,
horodatage, épinglables à une entité), mises en page avec plot PDF à
l'échelle exacte.

**3D façon Fusion** — la vue 3D est un périphérique d'entrée : survoler une
face définit le plan de travail et pilote un fantôme d'aperçu (rouge =
matière retirée, bleu = ajoutée) ; boîte de sélection au drag gauche,
orbite au drag droit / clic droit court = menu contextuel en arborescence
(Hole ▸/Face ▸/Edges ▸/Move ▸/Select ▸) ; Alt+clic ouvre un résolveur de
candidats qui surligne chaque option — y compris un aperçu « rayon X » qui
brille **à travers** les solides occultants ; ViewCube ; SPLIT/COMBINE de
solides selon un plan ou une surface courbe ; `FeatureTree` paramétrique
(trou/coque/extrusion…) éditable depuis le panneau Propriétés.

**Esquisses (sketches) v1** — légères, nommées, posées sur n'importe quelle
face ou plan de travail, visibles en 3D, **sans dépendance lourde** entre
une esquisse et le solide déjà généré à partir d'elle (éditer une esquisse
utilisée ne régénère jamais la pièce).

**Assemblage multi-STEP** — commande `ASSEMBLY`, panneau d'arborescence,
couleur/transparence par solide, insertion de plusieurs fichiers STEP en
une seule sélection.

**Fabrication PCB (Gerber/Excellon)** — lire, inspecter, mesurer, éditer et
réexporter des fichiers de fabrication **sans passer par un gros EDA**.
Ouverture d'un kit complet (dossier ou fichier seul, GUI/CLI/IPC) avec un
calque par couche et rendu fidèle de la polarité négative (LPC) ; pile de
couches façon CAM (transparence, ordre de dessin, rôle par calque, vue
miroir côté soudure) ; mesure de dégagement bord-à-bord (`MINDIST`) ;
inspecteur d'apertures ; rapports de perçage ; écriture RS-274X/Excellon
vérifiée **pixel par pixel contre `gerbv`** (le renderer de référence) sur
de vraies cartes ; `PANELIZE` ; pont DXF↔Gerber (un contour de carte
dessiné en 2D s'exporte en `.GKO` propre). Détails : [docs/PCB_CAM.md](docs/PCB_CAM.md).

**Export** — STL/OBJ, plots PDF, DXF, STEP, kits Gerber/Excellon complets ou
calque par calque.

### Compilation

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    libocct-foundation-dev libocct-modeling-algorithms-dev \
    libocct-modeling-data-dev libocct-data-exchange-dev \
    libocct-visualization-dev libocct-ocaf-dev \
    catch2 libzstd-dev libsqlite3-dev
scripts/build-and-test.sh
```

### CLI pour les agents

```sh
vikicad-cli new --run drawing.vks --save-as part.vkd
vikicad-cli open part.vkd --exec "CIRCLE 50,50 10" --exec "EXTRUDE 20 1" --save
vikicad-cli query part.vkd --entities --layers --notes --blocks --layouts
vikicad-cli query part.vkd --describe          # volume/aire/bbox/centroïde/features, en JSON
vikicad-cli export part.vkd out.dxf --dxf-version 2013
vikicad-cli export part.vkd out.step
vikicad-cli export kit.vkd fab/                # kit Gerber/Excellon complet
vikicad-cli import legacy.dxf --save-as legacy.vkd
vikicad-cli import fab-outputs/ --save-as board.vkd   # dossier = kit Gerber
vikicad-cli connect exec "LINE 0,0 100,0"      # pilote la GUI en cours d'exécution
```

Toute la sortie est du JSON sur une ligne : `{"ok":true,"result":{...}}`.

Le guide complet pour un agent — grammaire des commandes, adressage des
sous-formes (indices INSPECT), un exemple de modélisation de bout en bout
vérifié, la boucle de vérification visuelle et les pièges connus — est
**[docs/AGENT.md](docs/AGENT.md)**.

### Licence

VikiCAD est un logiciel libre, distribué sous **GNU General Public License,
version 3 ou (à ton choix) toute version ultérieure** (GPLv3-or-later). Voir
[LICENSE](LICENSE) pour le texte complet.

L'œuvre combinée est distribuée sous GPLv3 car la bibliothèque DXF vendorée
(libdxfrw) est GPLv2-or-later. Licences des dépendances :

| Dépendance | Licence | Liaison |
|---|---|---|
| libdxfrw (vendoré, patché) | GPL-2.0-or-later | statique |
| OpenCASCADE (OCCT) | LGPL-2.1 avec exception OCCT | dynamique |
| Qt 6 | LGPL-3.0 | dynamique |
| SQLite | Domaine public | dynamique |
| Catch2 | BSL-1.0 | tests uniquement |
| LibreDWG (`dwg2dxf`) | GPL-3.0 | binaire externe invoqué à l'exécution, non lié |

### Documentation

- `docs/DEVLOG.md` — journal de développement (une entrée par étape).
- `docs/LESSONS.md` — journal des pièges rencontrés et leçons retenues.
- `docs/PCB_CAM.md` — plan et état du chantier Gerber/Excellon.
- `docs/AGENT.md` — guide complet de pilotage headless.
- `third_party/libdxfrw/patches/` — patchs de la bibliothèque vendorée.

---

## English

2D drafting + 3D solids + PCB fabrication file (Gerber/Excellon) editing,
for Linux. Started as a personal replacement for nanoCAD, designed from day
one to be as usable by AI agents as by a mouse.

- **Stack**: C++17, Qt6 Widgets, OpenCASCADE 7.9, native SQLite format
  (`.vkd`), CMake.
- **Interop**: DXF R12–2018 (vendored, patched libdxfrw), DWG import (via
  `dwg2dxf`/LibreDWG), STEP (OCCT, notes as sidecar JSON or optional AP242
  attributes), STL/OBJ, PDF, and **Gerber RS-274X + Excellon** read/write.
- **Agent-friendly by design**: headless `vikicad-cli` with JSON output,
  JSON-RPC socket on the running GUI (`vikicad-cli connect`), `.vks` command
  scripts (AutoCAD `.scr` semantics). See [docs/AGENT.md](docs/AGENT.md).

### Features

**2D drafting** — LINE/CIRCLE/ARC/RECT/PLINE/ELLIPSE/SPLINE/POINT/XLINE;
MOVE/COPY/ROTATE/MIRROR/SCALE/STRETCH; TRIM/EXTEND/OFFSET/FILLET/CHAMFER/
BREAK/JOIN/EXPLODE; object snaps (endpoint/midpoint/center/quadrant/
intersection/perpendicular) + ORTHO/POLAR/GRID; vertex grips; layers;
one-click mm/inch toggle (`x,y`, `@dx,dy`, `@dist<angle`, `2"`, `10mm`).

**Annotation** — MTEXT, 5 dimension kinds regenerated live from DimStyle and
display units, leaders, hatches (SOLID/ANSI31/ANSI37), MATCHPROP.

**Organization** — blocks with attributes, associative rect/polar arrays
(live-editable via ARRAYEDIT), **sticky notes** (markdown, author,
timestamps, pinnable to entities), layouts with exact-scale PDF plotting.

**Fusion-style 3D** — the 3D view is an input device: hovering a face sets
the work plane and drives a ghost preview (red = material removed, blue =
added); box-select drag, right-drag orbit / short right-click = a
tree-structured context menu (Hole ▸/Face ▸/Edges ▸/Move ▸/Select ▸);
Alt+click opens a candidate resolver that highlights each option — including
an X-ray ghost that glows *through* occluding solids; ViewCube; SPLIT/
COMBINE solids by a plane or curved face; a parametric `FeatureTree`
(hole/shell/extrude…) editable from the Properties panel.

**Sketches v1** — lightweight, named, drawn on any face or work plane,
visible in the 3D view, with **no heavy dependency** between a sketch and
the solid already generated from it (editing a used-up sketch never
regenerates the part).

**Multi-STEP assemblies** — `ASSEMBLY` command, assembly tree panel,
per-solid color/transparency, multi-file selection when inserting STEP
components.

**PCB fabrication editor (Gerber/Excellon)** — read, inspect, measure, edit
and re-export fabrication files **without a full EDA suite**. Open a whole
kit (directory or single file, GUI/CLI/IPC) with one layer per file and
correct negative (LPC) polarity rendering; a CAM-style layer stack
(per-layer transparency, paint order, role, mirrored bottom view);
edge-to-edge clearance measurement (`MINDIST`); an aperture inspector; drill
reports; an RS-274X/Excellon writer checked **pixel-for-pixel against
`gerbv`** (the reference renderer) on real boards; `PANELIZE`; and a
DXF↔Gerber bridge (draw a board outline in 2D, export it as a clean
`.GKO`). Details: [docs/PCB_CAM.md](docs/PCB_CAM.md).

**Export** — STL/OBJ, PDF plots, DXF, STEP, whole Gerber/Excellon kits or a
single layer.

### Build

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    libocct-foundation-dev libocct-modeling-algorithms-dev \
    libocct-modeling-data-dev libocct-data-exchange-dev \
    libocct-visualization-dev libocct-ocaf-dev \
    catch2 libzstd-dev libsqlite3-dev
scripts/build-and-test.sh
```

### CLI for agents

```sh
vikicad-cli new --run drawing.vks --save-as part.vkd
vikicad-cli open part.vkd --exec "CIRCLE 50,50 10" --exec "EXTRUDE 20 1" --save
vikicad-cli query part.vkd --entities --layers --notes --blocks --layouts
vikicad-cli query part.vkd --describe          # volume/area/bbox/centroid/features, JSON
vikicad-cli export part.vkd out.dxf --dxf-version 2013
vikicad-cli export part.vkd out.step
vikicad-cli export kit.vkd fab/                # a full Gerber/Excellon kit
vikicad-cli import legacy.dxf --save-as legacy.vkd
vikicad-cli import fab-outputs/ --save-as board.vkd   # a directory = a Gerber kit
vikicad-cli connect exec "LINE 0,0 100,0"      # drive the running GUI
```

All output is single-line JSON: `{"ok":true,"result":{...}}`.

The complete agent guide — command grammar, sub-shape addressing (INSPECT
indices), a verified end-to-end modeling example, the verification loop and
the gotchas — is **[docs/AGENT.md](docs/AGENT.md)**.

### License

VikiCAD is free software, released under the **GNU General Public License,
version 3 or (at your option) any later version** (GPLv3-or-later). See
[LICENSE](LICENSE) for the full text.

The combined work is distributed under GPLv3 because the vendored DXF
library (libdxfrw) is GPLv2-or-later. Dependency licenses:

| Dependency | License | Linkage |
|---|---|---|
| libdxfrw (vendored, patched) | GPL-2.0-or-later | statically linked |
| OpenCASCADE (OCCT) | LGPL-2.1 with OCCT exception | dynamically linked |
| Qt 6 | LGPL-3.0 | dynamically linked |
| SQLite | Public domain | dynamically linked |
| Catch2 | BSL-1.0 | tests only |
| LibreDWG (`dwg2dxf`) | GPL-3.0 | external binary invoked at runtime, not linked |

### Docs

- `docs/DEVLOG.md` — development journal (one entry per milestone).
- `docs/LESSONS.md` — running log of mistakes and lessons learned.
- `docs/PCB_CAM.md` — plan and status of the Gerber/Excellon chantier.
- `docs/AGENT.md` — the complete headless-driving guide.
- `third_party/libdxfrw/patches/` — vendored-library patches.
