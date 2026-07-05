# MemViz - Windows Memory Visualizer

MemViz attaches to any running Windows process and renders its virtual address
space as a live, zoomable grid where **each cell is a 1 KB block**. Blocks can be
colored two ways:

- **Region metadata** - state (free/reserved/committed), protection (R/W/X,
  guard), and type (image/mapped/private), read from `VirtualQueryEx`. Cheap; no
  memory contents are read.
- **Byte content** - each block's actual bytes are read with `ReadProcessMemory`
  and colored by Shannon **entropy** and **zero-ratio** (low = blue, high = red,
  all-zero = near black, unreadable = dark red).

The map updates on a timer, and cells whose contents changed since the last scan
are highlighted.

## How it handles the huge address space

64-bit user space is ~128 TB and mostly empty, so MemViz never draws a cell per
address. It enumerates only **reserved + committed** regions, packs them
end-to-end, and wraps them into a grid `width` cells wide. At 1 KB per cell the
grid can still be enormous, so if it would exceed the D3D11 max texture size
(16384 rows) MemViz automatically aggregates multiple KB per cell (shown as
"Cell size" in the stats panel). For typical processes it stays at 1 KB/cell.

## Features

- Attach by picking from the live process list (filter by name or PID).
- Metadata / content coloring toggle.
- Zoom (mouse wheel or slider) and pan (right-drag).
- Hover tooltip: address, region base/size, state, protection, type, module.
- Click a cell to inspect it: entropy, zero-ratio, and a hex preview.
- Auto-refresh with configurable interval and change highlighting.
- Selectable row width (256 / 512 / 1024).

## Build

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 Build Tools (MSVC) + Windows SDK
- CMake 3.20+
- Network access on first configure (Dear ImGui is fetched via `FetchContent`).

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is written to `build/Release/memviz.exe`.

## Run

```bash
build/Release/memviz.exe
```

Reading another process's memory requires `PROCESS_VM_READ`. For system or
elevated targets, **run MemViz as administrator** (it also enables
`SeDebugPrivilege` at startup). If a target cannot be opened you'll see an
`OpenProcess failed` message in the status bar; regions that can't be read are
drawn as "Unreadable" in content mode.

## Notes / limitations

- Build and run **x64**; a 64-bit target is only reliably readable from a 64-bit
  build (the architectures must match).
- Anti-cheat / DRM-protected processes may block `OpenProcess` or reads. This is
  expected.
- Content coloring is bounded by a per-scan byte budget (256 MB) so it stays
  responsive; beyond that, committed cells show as "Not sampled".

## Project layout

| File | Purpose |
|------|---------|
| `src/memscan.*` | Win32 access: process/module enumeration, region walk, reads, privileges |
| `src/model.*`   | Regions -> 1 KB cell grid + metadata/content color buffer |
| `src/gfx.*`     | DirectX 11 device + dynamic texture upload |
| `src/app.cpp`   | Win32 window, ImGui UI, scanning worker thread |
