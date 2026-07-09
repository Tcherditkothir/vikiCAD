# Contributing to VikiCAD

## Build

Dependencies (Debian/Ubuntu):

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev \
    libocct-foundation-dev libocct-modeling-algorithms-dev \
    libocct-modeling-data-dev libocct-data-exchange-dev \
    libocct-visualization-dev libocct-ocaf-dev \
    catch2 libzstd-dev libsqlite3-dev
```

Debug build (the default developer workflow):

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j$(nproc)
```

Release build:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)
```

Or simply run `scripts/build-and-test.sh`.

## Tests

The suite is Catch2. Run it after every change; it must stay all-green:

```sh
./build/debug/tests/vikicad-tests
```

New test files go in `tests/` and must be registered in
`tests/CMakeLists.txt`.

## Code style

- C++17, Qt6 Widgets, OpenCASCADE. Match the surrounding code.
- **Geometry is in millimetres everywhere.** Unit conversion happens only at
  parse/display boundaries.
- **Commands take numeric parameters before entity selection** (the entity
  set is greedy); keep new commands consistent with this order.
- **Every document mutation goes through a transaction** — undo/redo then
  works automatically. Never mutate entities outside one.
- OCCT pitfall: `Make*::IsDone()` is unreliable; call `.Build()`/`.Shape()`
  and null-check the result instead.

## Docs

Internal documentation under `docs/` (DEVLOG, LESSONS, REPRISE...) is written
in French. Contributions to it may be in French or English.

## License

By contributing you agree that your work is released under the
GNU General Public License v3.0 or later (see `LICENSE`).
