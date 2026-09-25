# Vantage Configuration

Vantage uses a simple INI-like keyfile format. The user's config lives
at `~/.config/vantage/vantage.conf` and system-wide defaults at
`/usr/local/etc/vantage/vantage.conf`.

## Sections and keys

### `[desktop]` — global desktop settings

| Key              | Default        | Description                              |
|------------------|----------------|------------------------------------------|
| `backend`        | `auto`         | wayland / x11 / auto (`xorg` and `xlibre` accepted as `x11` aliases) |
| `renderer`       | `auto`         | opengl / vulkan / software / auto         |
| `theme`          | `Vantage-Dark` | Name of installed theme                  |
| `dark-mode`      | `true`         | Apply dark variant                       |
| `wallpaper`      | (empty)        | Path to image (or shader code)           |
| `video-wallpaper`| (empty)        | Path to video for live wallpaper         |
| `video-volume`   | `0`            | 0..1 (0 = muted)                         |
| `compositor`     | `true`         | Enable compositor                         |
| `animations`     | `true`         | Enable window animations                 |
| `vsync`          | `true`         | VSync (when supported)                    |
| `shadows`        | `true`         | Enable window shadows                    |
| `blur`           | `false`        | Enable background blur (expensive)        |
| `scale`          | `1.0`          | Global UI scale (per-output overrides)    |
| `workspace-count`| `4`            | Number of workspaces                      |
| `panel-height`   | `32`           | Default panel height in px               |
| `panel-position` | `top`          | top/bottom/left/right                    |
| `dbus`           | `auto`         | auto / enabled / disabled                 |
| `log-level`      | `info`         | trace/debug/info/notice/warn/error/crit   |

### `[wm]` — window manager settings

| Key          | Default | Description                          |
|--------------|---------|--------------------------------------|
| `focus-new`  | `true`  | Focus newly-created windows          |
| `tile-key`   | `Super+Direction` | Key combo for tiling         |

### `[panel]` — panel settings

| Key        | Default | Description                          |
|------------|---------|--------------------------------------|
| `position` | `top`   | top/bottom/left/right                |
| `height`   | `32`    | Panel height in pixels               |

### `[audio]`

| Key        | Default | Description                          |
|------------|---------|--------------------------------------|
| `backend`  | `auto`  | pipewire / pulse / alsa / auto / none |

### `[network]`

| Key        | Default | Description                          |
|------------|---------|--------------------------------------|
| `backend`  | `auto`  | nm / connman / proc / auto / none    |

### `[power]`

| Key        | Default | Description                          |
|------------|---------|--------------------------------------|
| `backend`  | `auto`  | logind / upower / ioctl / auto / none |

### `[displays]` and `[output:NAME]`

Per-monitor config keys: `enabled`, `x`, `y`, `scale`, `mode`.

```ini
[output:HDMI-1]
enabled=true
x=0
y=0
scale=1.0

[output:DP-1]
enabled=true
x=1920
y=0
scale=2.0
```

## Editing config

### CLI

```sh
vantage-config init             # write defaults to ~/.config/vantage/vantage.conf
vantage-config get desktop theme
vantage-config set desktop theme Vantage-Light
vantage-config list             # print all keys
```

### By hand

Edit `~/.config/vantage/vantage.conf` with any text editor. Vantage
watches the file via inotify and reloads live (when using `vt_config_watch()`).

## Defaults

To see all default values:

```sh
vantage-config get-defaults
```

## Environment variables

These override the config file:

| Var                    | Description                              |
|------------------------|------------------------------------------|
| `VANTAGE_LOG_LEVEL`   | trace/debug/info/notice/warn/error/crit  |
| `VANTAGE_QSS`         | Path to Qt6 QSS file (set by theme engine) |
| `VANTAGE_BACKEND`     | `wayland` or `x11` — same choice as the session CLI flags |
| `__GLX_VENDOR_LIBRARY_NAME` | EGL vendor (set by gpu module)      |
| `GTK_THEME`           | GTK theme (set by theme engine)          |
| `QT_QPA_PLATFORMTHEME`| Qt platform theme (set to `vantage`)    |
| `QT_QPA_PLATFORM`     | Qt backend list (xcb;wayland)            |
