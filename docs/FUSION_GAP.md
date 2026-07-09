# VikiCAD → écart avec l'ergonomie Fusion 360

Feuille de route pour rapprocher VikiCAD d'une CAO paramétrique moderne
(Fusion 360). Sert de **liste de travail pour le workflow nocturne autonome**.

Chaque item porte :
- **[AUTO]** = implémentable + vérifiable **sans souris** (tests unitaires
  Catch2 + IPC `pick3d`/`sketchface`/`view3d` + dump OCCT). Priorité nuit.
- **[GUI]** = nécessite une validation visuelle/souris → coder + build + test
  headless du cœur, mais **Lex valide** le ressenti au réveil.
- Priorité P0 (le plus fort levier) → P3.

## Contexte technique (déjà en place, réutiliser)
- Solides = `SolidEntity` (wrapper `TopoDS_Shape`), BREP dans .vkd + undo.
- `core/solid/SolidOps.*` : extrude/revolve/boolean/pushPullFace/planeFromFace/
  faceOutline2d ; `WorkPlane` = repère orthonormé ; `documentWorkplane(doc)`.
- Vue 3D `OcctViewWidget` : pick (`pickAtPhysical`/`pickCenter`), highlight
  cyan/orange, DPR géré, `pickedFace()`/`pickedSolid()`.
- IPC utile pour tester : `view3d on/off`, `pick3d x y`, `sketchface`,
  `insertstep`, `screenshot` (dump OCCT en 3D), `exec`, `query`.
- Commandes 3D : EXTRUDE/REVOLVE/UNION/SUBTRACT/INTERSECT/FILLET3D/MOVE3D/
  ROTATE3D/TRANSPARENCY, WORKPLANE XY|OFFSET, GEAR.

---

## 1. Esquisse (sketch) — le cœur de Fusion

- **P0 [AUTO] Snap sur la référence de face en sketch.** Le contour projeté
  (`faceOutline2d`) est dessiné mais pas accrochable. Injecter ses sommets/
  centres de cercles comme cibles de snap pendant le sketch (endpoint/center).
  Test : snapQuery renvoie les points du contour.
- **P0 [AUTO] Contraintes de sketch géométriques** : coïncidence, horizontal/
  vertical, parallèle, perpendiculaire, tangent, égal, concentrique. Solveur
  2D léger (ex. réseau de contraintes + Newton, ou intégrer `planegcs`/
  solveur maison). GROS morceau — commencer par H/V + coïncidence + parallèle.
  Test : un rectangle sous contraintes reste rectangle après déplacement d'un
  point.
- **P1 [AUTO] Cotes de sketch pilotantes** (driving dimensions) : une cote
  fixe une distance/rayon/angle ; éditer la cote met à jour la géométrie.
  Dépend du solveur.
- **P1 [GUI] Vue de sketch réorientée** : passer en sketch aligne la caméra
  face au plan (regarde selon −normale), grille dans le plan. (Aujourd'hui on
  dessine dans le canevas 2D XY abstrait.)
- **P2 [AUTO] Profils ouverts & régions auto** : détecter les régions fermées
  d'un ensemble de courbes qui se croisent (comme Fusion) au lieu d'exiger une
  boucle unique fermée. Test : deux cercles sécants → 3 régions.
- **P2 [AUTO] Offset/trim/extend de sketch dans le plan** (déjà en 2D monde ;
  vérifier que ça marche relatif au workplane).

## 2. Modélisation solide (features)

- **P0 [AUTO] Historique de features paramétrique** : arbre EXTRUDE→FILLET→…
  rejouable ; éditer une feature recalcule l'aval. C'EST le cœur de Fusion.
  Refonte majeure : `SolidEntity` devient le résultat d'un arbre de features
  (`FeatureNode` : sketch, extrude, pocket, fillet, hole, pattern…). Stocker
  l'arbre dans le .vkd, régénérer le BREP. Commencer petit : sketch + extrude
  éditables. Test : changer la hauteur d'extrude régénère le volume.
- **P0 [AUTO] Extrude avec options** : symétrique, deux côtés, jusqu'à une
  face/surface (up-to), coupe (cut) vs jointure (join) vs nouveau corps.
  `extrudeWires` a la base ; ajouter les modes. Test volumes.
- **P0 [AUTO] Trou (Hole) paramétrique** : diamètre, profondeur, débouchant,
  fraisé/chanfreiné, taraudé (cosmétique). = pocket cylindrique. Test.
- **P1 [AUTO] Congé/chanfrein par ARÊTE choisie** (pas toutes les arêtes).
  FILLET3D actuel prend toutes les arêtes ; permettre la sélection d'arêtes
  (via `pick3d` mode edge → owner shape). BRepFilletAPI sur une liste d'arêtes.
  Test : congé d'une arête d'une boîte réduit le volume d'une quantité connue.
- **P1 [AUTO] Coque (Shell)** : évider un solide en gardant une épaisseur,
  face(s) ouverte(s). BRepOffsetAPI_MakeThickSolid. Test volume.
- **P1 [AUTO] Répétition (Pattern) 3D** : rectangulaire, circulaire, sur
  chemin. (2D ARRAYRECT/POLAR existent ; porter en 3D via MOVE3D des corps.)
- **P2 [AUTO] Balayage (Sweep) & lissage (Loft)** : BRepOffsetAPI_MakePipe /
  ThruSections. Test volumes simples.
- **P2 [AUTO] Draft (dépouille), Rib, Web, Thread réel.**

## 3. Assemblage

- **P0 [AUTO] Contraintes d'assemblage (joints/mates)** : coïncidence de
  faces, alignement d'axes, distance, angle. Calculer le `gp_Trsf` qui place un
  composant pour satisfaire la contrainte (ex. face A sur face B). Commencer
  par « mate deux faces planes » (coïncidence + anti-normale) et « axe sur
  axe ». Test : après mate, les deux faces sont coplanaires (distance ~0).
- **P1 [GUI] Gizmo de déplacement 3D** (translation/rotation à la souris avec
  poignées) au lieu de taper dx/dy/dz.
- **P1 [AUTO] Arbre d'assemblage hiérarchique** (sous-assemblages, instances
  d'un même composant). Aujourd'hui : composants plats nommés.
- **P2 [AUTO] Détection d'interférences** (BRepAlgoAPI_Common volume>0 entre
  composants). Test.
- **P2 [AUTO] Motion/contact** (hors périmètre v1).

## 4. Navigation & UX 3D

- **P0 [GUI] ViewCube / vues normalisées** : Top/Front/Right/Iso, « aligner la
  vue sur la face sélectionnée ». `V3d_View::SetProj` + un widget coin.
  (`view3d` + un verbe `viewalign` testable au dump.)
- **P1 [GUI] Sélection persistante multi + fenêtre/box select en 3D.**
- **P1 [GUI] Mesure 3D** (distance entre 2 faces/arêtes/sommets, angle).
  [AUTO le cœur : BRepExtrema_DistShapeShape ; test.]
- **P1 [GUI] Section/coupe dynamique** (plan de clip). `V3d_View` clip planes.
- **P2 [GUI] Apparences/matériaux réalistes, ombres, éclairage d'environnement.**
- **P2 [GUI] Explosion d'assemblage.**

## 5. Paramétrique & données

- **P1 [AUTO] Table de paramètres utilisateur** (d = 10, largeur = 2*d) pilotant
  cotes et features. Petit évaluateur d'expressions. Test.
- **P2 [AUTO] Export mesh (STL/OBJ) et 3MF** pour l'impression 3D (Lex fait de
  l'impression). BRepMesh + écriture STL. Test : cube → STL de 12 triangles.
- **P2 [AUTO] Dérivation dessin 2D depuis le 3D** (mise en plan : vues
  projetées + cotes). HLR (Hidden Line Removal) OCCT `HLRBRep`.

## 6. Dettes / robustesse (à traiter en passant)
- [AUTO] Snap tangent/nearest/node (manque au SnapEngine).
- [AUTO] Word-wrap MTEXT par largeur de colonne ; suffixe DIMSTYLE (dimpost).
- [AUTO] `documentWorkplane`/sketch-ref : nettoyer quand on quitte le sketch ;
  persister le workplane courant dans le .vkd.
- [GUI] `pushPull`/`sketchOnFace` : garder la sélection surlignée après l'op.

---

## Ordre recommandé pour la nuit (maximiser valeur × testabilité headless)
1. **Trou paramétrique + Extrude cut/symmetric/up-to** [AUTO] — features solides
   très demandées, testables au volume.
2. **Congé/chanfrein par arête sélectionnée** [AUTO].
3. **Coque (Shell)** [AUTO].
4. **Contrainte d'assemblage « mate 2 faces » + « axe sur axe »** [AUTO].
5. **Export STL/3MF** [AUTO] (utile impression 3D).
6. **Mesure 3D (DistShapeShape)** cœur [AUTO].
7. **Snap sur la référence de face** [AUTO].
8. **Historique de features minimal (sketch+extrude éditables)** [AUTO] — gros,
   à attaquer si le reste avance ; sinon poser les fondations + tests.
9. **ViewCube + viewalign** [GUI, cœur testable].

Chaque item : coder dans `core/` (testable), commande + verbe IPC si utile,
**test Catch2**, build vert, commit atomique avec message clair. Laisser les
items purement visuels (gizmo, matériaux) pour validation de Lex.
