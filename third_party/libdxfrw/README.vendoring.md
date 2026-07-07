# Vendoring libdxfrw

Source: https://github.com/LibreCAD/libdxfrw (LibreCAD fork).

To vendor (requires network):

```sh
git clone --depth 1 https://github.com/LibreCAD/libdxfrw.git /tmp/libdxfrw
rsync -a --exclude .git /tmp/libdxfrw/ third_party/libdxfrw/
```

Then add a minimal `CMakeLists.txt` here building a static `dxfrw` target from
`src/*.cpp` (the upstream build may need adaptation), and commit.

Local patches go in `patches/` (git format-patch), each one logged in
`docs/DEVLOG.md` with its reason.

The top-level build treats this directory as optional: if `src/` is missing,
VikiCAD compiles without DXF support (`VIKICAD_HAS_DXF=OFF`).
