# Vendored libdxfrw

**Provenance:** LibreCAD fork of libdxfrw, extracted from the Ubuntu source
tarball `librecad_2.2.0.2.orig.tar.gz` (path `libraries/libdxfrw/src`),
downloaded 2026-07-07 from
http://archive.ubuntu.com/ubuntu/pool/universe/libr/librecad/
(GitHub was unreachable — IPv6-only network; the Ubuntu pool is the same code,
pinned to LibreCAD's 2.2.0.2 release).

Upstream: https://github.com/LibreCAD/libdxfrw — compare against it before
upgrading.

Built as static target `dxfrw` by the local `CMakeLists.txt`.

Local patches go in `patches/` (git format-patch), each one logged in
`docs/DEVLOG.md` with its reason. No patches applied yet.

The top-level build treats this directory as optional: if `src/` is missing,
VikiCAD compiles without DXF support (`VIKICAD_HAS_DXF=OFF`).
