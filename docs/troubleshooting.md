# Vantage Troubleshooting

## Quick first step

Always start with:

```sh
vantage-diagnostics
```

This shows what backend, renderer, GPU, audio/network/power backends,
init system, and D-Bus are detected. If hardware accel shows
`disabled`, see [GPU acceleration](gpu.md).

## Common issues

### Black screen / nothing happens

1. **No GPU detected** — Vantage falls back to the software renderer
   in headless mode. To use X11:

   ```sh
   export DISPLAY=:0
   vantage-config set desktop backend xorg
   ```

2. **No DISPLAY or WAYLAND_DISPLAY** — Vantage cannot connect to any
   X server or Wayland compositor. Start one first (`startx`,
   `sway`, `greetd`).

3. **Qt6/GTK theming not applied** — Run `vantage-theme apply` and
   restart your apps. Verify with `vantage-theme info <name>`.

### Performance issues

* **Video wallpaper stutter** — Your GPU may not have hardware video
  decode. Check `vantage-diagnostics` for `Video decode: ENABLED`.
  Disable with `vantage-config set desktop video-wallpaper ""`.

* **High CPU** — Check if the panel has volume/network applets that
  are polling in a loop. The current code polls once per frame which
  is too often; in a future release these will use event-driven APIs.

* **Animations janky** — Disable vsync to verify:
  `vantage-config set desktop vsync false`. If that fixes it, you
  have a driver vsync bug; report upstream.

### OpenGL / EGL issues

* `Renderer: software` — means EGL was not detected at build time.
  Install `libegl-dev` (or `mesa-libEGL-devel` on Fedora) and
  re-run `meson setup build --reconfigure`.

* `egl: false` in build summary — see above.

* OpenGL renderer selected but no rendering — Check `DISPLAY` or
  `WAYLAND_DISPLAY`. The renderer needs a surface from the backend.

### NVIDIA-specific issues

* EGL not initializing under Wayland — make sure
  `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json`
  exists (installed by NVIDIA driver).

* Flickering on XWayland apps — known issue with EGL Streams on
  older drivers. Update to driver version 525+ or use the Xorg backend:
  `vantage-config set desktop backend xorg`.

* Multi-monitor glitch — Use `xrandr` to confirm monitors are detected,
  then re-run Vantage.

### Config not saving

* Check that `~/.config/vantage/` is writable.
* Use `vantage-config path` to see the exact path used.
* Check the log level:
  ```sh
  VANTAGE_LOG_LEVEL=debug vantage-session
  ```

### Wayland session not appearing in display manager

After `meson install`, the desktop file
`/usr/share/xsessions/vantage.desktop` should be installed. Run
`update-desktop-database` (Debian) or equivalent to refresh the menu.
Restart your display manager (gdm/sddm/lightdm).

### D-Bus errors

Vantage does not require D-Bus. If you see "D-Bus: unavailable" in
diagnostics, that's fine — Vantage falls back to its own Unix-socket
IPC and direct /sys reads for power/network.

To enable D-Bus integration:
```sh
sudo apt install libdbus-1-dev
meson setup build --reconfigure -Ddbus=enabled
ninja -C build && sudo ninja -C build install
```

### Theme not applying

1. Verify the theme JSON exists:
   ```sh
   vantage-theme list
   ```
2. Check for syntax errors:
   ```sh
   vantage-theme info Vantage-Dark
   ```
3. Check `~/.config/gtk-3.0/gtk.css` was generated.
4. Make sure `GTK_THEME=vantage` is set in the environment.

### Window manager shortcuts not firing

1. Verify your keyboard layout is correctly detected (look in
   `vantage-diagnostics`).
2. Check if you have a third-party hotkey daemon (e.g. `sxhkd`,
   `xbindkeys`) running that's stealing the keys.

### `Wayland backend: built without libwayland support`

This means Vantage was built without libwayland. Install
`libwayland-dev` and rebuild with `meson setup build --reconfigure`.

### No audio

Check `vantage-diagnostics` for the audio backend. If it says "none":

1. Install one of: `pipewire`, `pulseaudio`, `alsa-utils`.
2. Make sure the user is in the `audio` group (Debian/Ubuntu).
3. For PipeWire specifically, the user-level service must be running
   (`systemctl --user status pipewire`).

## Logs

Logs go to stderr by default. To capture them:

```sh
vantage-session 2>&1 | tee /tmp/vantage.log
```

To increase verbosity:

```sh
VANTAGE_LOG_LEVEL=debug vantage-session
# or:
VANTAGE_LOG_LEVEL=trace vantage-session
```

## Reporting bugs

Please include:

1. `vantage-diagnostics` output
2. `vantage-config list` output
3. The Vantage version: `vantage-config --version` (TODO)
4. Your distribution and desktop environment
5. The exact reproduction steps
