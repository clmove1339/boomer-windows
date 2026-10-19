# boomer for Windows

A Windows take on [tsoding/boomer](https://github.com/tsoding/boomer): zoom into your screen, pixelated, with a flashlight.
It's one C file using Win32, GDI and DXGI, with no dependencies, and builds to a ~170 KB exe.

## Build

Needs Visual Studio 2022 with the C++ workload:

```bat
build.bat
```

## Run

```bat
boomer.exe            :: open once, Esc quits
boomer.exe --daemon   :: stay in the background, pop up on Win+Shift+Z
```

To start the daemon on login, put a shortcut to `boomer.exe --daemon` into `shell:startup`.

## Controls

| Input | Action |
|---|---|
| Mouse wheel | Zoom toward the cursor |
| Left drag | Pan |
| `F` | Toggle flashlight (fades in/out) |
| Ctrl + wheel | Flashlight radius |
| `0` | Reset view |
| `Q` / `Esc` | Zoom back to 1:1 and close (press again to skip) |

## How it works

- **Capture:** DXGI Desktop Duplication on every monitor, with a GDI fallback. The image stays live while open, and boomer's own window is excluded from capture.
- **Pixelated zoom:** `StretchBlt` with `COLORONCOLOR` (nearest neighbour).
- **Monitors:** The layout is re-read on every popup, so plugging monitors in needs no restart.
- **Idle cost:** While hidden, the daemon uses no timer and frees its screen buffers. It keeps the D3D device so popup takes ~20–30 ms.

Known limits: rotated monitors fall back to a static GDI capture. A monitor added while boomer is open shows up on the next popup.
