# linux-wallpaperengine usage (GNOME Shell / Wayland)

This backend runs the full rendering core in an offscreen EGL context and hands every frame to a GNOME Shell extension through POSIX shared memory. The extension paints the frames inside Mutter's background group. It does not use wlr-layer-shell and does not require switching to Xorg.

---

## 1. Requirements

- GNOME Shell 47 or newer (verified on GNOME Shell 50.1 / Mutter 18), Wayland session
- The engine built with `-DGNOME_SHELL_BACKEND=ON`
- Wallpaper Engine's assets (from Steam)
- A wallpaper ID from the Wallpaper Engine workshop, for example `2976403740`

## 2. Building

```bash
cmake -S . -B build -DGNOME_SHELL_BACKEND=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Output: `build/output/linux-wallpaperengine`

## 3. One-stop install and run (recommended)

```bash
# First run: installs the extension, writes and enables the systemd user unit
./packaging/gnome-shell/run.sh 2976403740

# Later runs without arguments reuse the last wallpaper(s)
./packaging/gnome-shell/run.sh
```

The script:

1. Copies the extension to `~/.local/share/gnome-shell/extensions/linux-wallpaperengine-gnome@local`
2. Generates `~/.config/systemd/user/linux-wallpaperengine-gnome.service`
3. Runs `systemctl --user enable`, so **the wallpaper comes back on every login**
4. Restarts the unit when the extension is already loaded, otherwise asks you to log out and back in once

After the first install, or whenever the extension files change, **log out and back in**. GNOME Shell only imports an extension module once; `gnome-extensions disable/enable` does not reload it from disk.

## 4. Wallpaper argument forms

```bash
# One wallpaper across the whole desktop
./packaging/gnome-shell/run.sh 2976403740

# One wallpaper per monitor; <index> is the 0-based monitor index
./packaging/gnome-shell/run.sh 0=2976403740 1=3023217614
```

- The plain form and the `<index>=<background>` form cannot be mixed
- The plain form accepts exactly one wallpaper
- With no arguments the script reads the previous configuration from `~/.config/linux-wallpaperengine-gnome/args`

## 5. Per-monitor wallpapers (engine directly)

`--gnome-shell-screen` is repeatable. It selects the GNOME Shell backend on its own and makes the following `--bg` / `--playlist` / `--scaling` / `--clamp` apply to that monitor:

```bash
linux-wallpaperengine \
    --gnome-shell-screen 0 --bg 2976403740 --scaling fill \
    --gnome-shell-screen 1 --bg 3023217614 --scaling fill
```

The monitor indices are printed at startup:

```
GNOME Shell desktop bounding box is 3840x1285 at (0,0) across 2 monitor(s), rendering frames at 3840x1285
  monitor 0 -> 1920x85:1920x1200
  monitor 1 -> 0x0:1920x1080
```

GNOME Shell's JS API does not expose connector names, so monitors are picked by index only.

## 6. Performance tuning

Every frame goes through: discrete GPU render → readback → shared memory → Shell copy → texture upload → composition. At 3840x1285 and 30 FPS that is roughly 1.2 GB/s, which is the main source of desktop stutter.

`--frame-scale` (0.1 - 1.0, default 1.0) renders and publishes the frames at a lower resolution; the extension upscales them while compositing:

```bash
# Through run.sh
LWE_FRAME_SCALE=0.75 ./packaging/gnome-shell/run.sh 2976403740

# Engine directly
linux-wallpaperengine --gnome-shell 2976403740 --frame-scale 0.75
```

Measured on a 3840x1285 span at 30 FPS (GNOME Shell idles at 24%):

| frame-scale | gnome-shell CPU | Notes |
|---|---|---|
| 1.0 | 67% | Original sharpness |
| 0.75 | 53% | Slightly softer, barely visible |
| 0.5 | 36% | Noticeably softer, smoothest |

Other ways to lower the load:

```bash
# Lower the frame rate
linux-wallpaperengine --gnome-shell 2976403740 --fps 24

# Disable particles / mouse interaction / parallax
linux-wallpaperengine --gnome-shell 2976403740 --disable-particles --disable-mouse --disable-parallax
```

The engine logs the GPU it actually uses, which is handy to confirm it runs on the discrete GPU:

```
GNOME Shell OpenGL renderer: NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2
```

## 7. Running manually (debugging)

Bypassing systemd:

```bash
XDG_SESSION_TYPE=wayland ./build/output/linux-wallpaperengine --gnome-shell 2976403740
```

Common options:

| Option | Purpose |
|---|---|
| `--gnome-shell <id>` | One wallpaper across the whole desktop |
| `--gnome-shell-screen <index>` | Selects the backend and the monitor the following wallpaper applies to; repeatable |
| `--bg <id>` | Wallpaper to use |
| `--scaling <mode>` | Scaling mode: `stretch`, `fit`, `fill`, `default` |
| `--clamp <mode>` | UV clamp mode: `clamp`, `border`, `repeat` |
| `--frame-scale <0.1-1.0>` | Publish frames at a lower resolution to reduce the Shell load |
| `--fps <n>` | Frame rate limit, 30 by default |
| `--assets-dir <path>` | Assets folder, when auto-detection fails |
| `--disable-particles` / `--disable-mouse` / `--disable-parallax` | Turn off the corresponding effects |
| `--list-properties` / `--set-property` | Inspect and override wallpaper properties |

## 8. Service management

```bash
# Status
systemctl --user status linux-wallpaperengine-gnome.service

# Live logs
journalctl --user -u linux-wallpaperengine-gnome.service -f

# Restart / stop / disable autostart
systemctl --user restart linux-wallpaperengine-gnome.service
systemctl --user stop linux-wallpaperengine-gnome.service
systemctl --user disable --now linux-wallpaperengine-gnome.service
```

Installing the extension and unit only (start them yourself):

```bash
./packaging/gnome-shell/install.sh [engine-binary] [wallpaper-path]
gnome-extensions enable linux-wallpaperengine-gnome@local
systemctl --user enable --now linux-wallpaperengine-gnome.service
```

## 9. Uninstalling

```bash
./packaging/gnome-shell/uninstall.sh
```

The script disables and removes the service, removes the extension, deletes the configuration directory and cleans up leftover `/dev/shm/lwe-gnome-*` segments. Log out and back in for the extension to be fully unloaded.

## 10. Known limitations

- **One logout is required**: extension modules are imported only at login, so updated extension files need a fresh login
- **Monitors are picked by index**: GNOME Shell's JS API has no connector names
- **No fullscreen pause**: Mutter does not implement `zwlr-foreign-toplevel`, so fullscreen windows cannot be detected on Wayland
- **Limited mouse interaction**: the extension only feeds the pointer position back; clicks are not supported
- **Not zero-copy**: every frame goes through a CPU copy; high resolution plus high frame rate visibly loads the iGPU and the Shell main thread
- Wallpapers are only rendered for monitors the extension reported; if nothing is reported the engine exits with an error
