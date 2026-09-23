# Qt6 Integration

Vantage themes Qt6 applications **without** making Qt6 a dependency of
the desktop core. The mechanism is one-way:

1. Vantage writes a QSS file: `~/.config/Vantage/vantage.qss`
2. Vantage sets `QT_QPA_PLATFORMTHEME=vantage` in the environment.
3. Qt6 apps launched from within the Vantage session inherit this
   env var and look for a platform theme plugin named `vantage`.

## Without the optional Qt6 plugin

If you don't install the optional `vantage-qt6platformthemeplugin.so`
plugin, Qt6 apps will fall back to their default platform theme but
**still** pick up the env vars:

* `QT_QPA_PLATFORM=xcb;wayland` (prefer X11, fall back to Wayland)
* `VANTAGE_QSS=<path>` (where the generated QSS lives)

To get the QSS applied without the plugin, run Qt6 apps with:

```sh
QT_QPA_PLATFORMTHEME= stylesheet=$VANTAGE_QSS myapp
```

## With the optional Qt6 plugin (recommended)

A separate `vantage-qt6` package provides the platform theme plugin
that:
* Loads `$VANTAGE_QSS` automatically.
* Forwards colors to Qt's standard palette.
* Applies the configured font.

(Docs will be updated when the Qt6 plugin is published.)

## Disabling Qt6 integration

```sh
unset QT_QPA_PLATFORMTHEME
```

Or simply don't run `vantage-theme apply`.
