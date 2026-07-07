# VikiCAD

2D drafting + 3D solid CAD for Linux. Personal replacement for nanoCAD.

- **Stack:** C++17, Qt6 Widgets, OpenCASCADE, SQLite native format (`.vkd`), CMake.
- **Interop:** DXF (libdxfrw, vendored) and STEP AP242 (OCCT) as import/export.
- **Agent-friendly:** headless `vikicad-cli` with JSON output, JSON-RPC socket on the GUI, `.vks` command scripts.

## Build

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    libocct-foundation-dev libocct-modeling-algorithms-dev \
    libocct-modeling-data-dev libocct-data-exchange-dev \
    libocct-visualization-dev libocct-ocaf-dev \
    catch2 libzstd-dev libsqlite3-dev
scripts/build-and-test.sh
```

Targets: `vikicad` (GUI), `vikicad-cli` (headless), `vikicad-tests` (Catch2).

## Docs

- `docs/DEVLOG.md` — development journal (one entry per milestone minimum).
- `docs/LESSONS.md` — running log of mistakes and lessons learned.
