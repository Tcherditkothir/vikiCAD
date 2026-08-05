# cgltf (vendored)

- **Provenance** : https://github.com/jkuhlmann/cgltf, tag `v1.15`,
  fichiers `cgltf.h` + `cgltf_write.h`, téléchargés le 2026-08-05.
- **Licence** : MIT (en-tête des fichiers).
- **Rôle ici** : lecteur glTF/GLB INDÉPENDANT pour les tests round-trip de
  l'export GLB du module d'animation (`core/anim/GlbExporter`). L'exporteur
  lui-même n'en dépend PAS (il écrit son JSON glTF via Qt et le conteneur
  GLB à la main) — la règle des LESSONS veut qu'un écrivain de fichier soit
  jugé à la relecture par un AUTRE lecteur, d'où cette lib.
- **Cible CMake** : `cgltf` (INTERFACE, include SYSTEM). Garde
  `VIKICAD_HAS_CGLTF` au CMakeLists racine : sans le dossier, le build passe
  et seuls les tests round-trip disparaissent (patron libdxfrw).
- **Patches** : aucun. Upgrade = remplacer les deux fichiers et relancer la
  suite.
