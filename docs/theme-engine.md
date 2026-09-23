# Vantage Theme Engine

Vantage has a **single JSON theme file** that drives both GTK and Qt6
theming. Apply one theme, both toolkits pick up the same look.

## Theme file format

A theme file is `~/.local/share/vantage/themes/<name>/theme.json`:

```json
{
  "name": "Vantage-Dark",
  "dark": true,
  "accent": "#3b82f6",
  "bg": "#1e1e2e",
  "fg": "#cdd6f4",
  "surface": "#313244",
  "border": "#45475a",
  "radius": 8,
  "spacing": 8,
  "font_family": "Sans",
  "font_size_pt": 11,
  "icon_theme": "Adwaita"
}
```

A simpler `key=value` format is also accepted (parsed with json-c
fallback):

```
dark=true
accent=#3b82f6
bg=#1e1e2e
...
```

## Applying a theme

```sh
vantage-theme apply Vantage-Dark
```

This:
1. Reads `<theme>/theme.json`.
2. Writes `~/.config/gtk-3.0/gtk.css` and `~/.config/gtk-4.0/gtk.css`
   with GTK3/4-compatible CSS derived from the JSON palette.
3. Writes `~/.config/Vantage/vantage.qss` with Qt6-compatible QSS.
4. Sets `GTK_THEME`, `GTK_APPLICATION_PREFER_DARK`,
   `QT_QPA_PLATFORMTHEME`, `VANTAGE_QSS` env vars.

## Generating a palette from a single color

```sh
vantage-theme generate "#3b82f6" --dark
vantage-theme generate "#3b82f6" --light
```

Vantage uses a HSL-based color generator to derive a full palette
(bg, fg, surface, border) from just the accent color. This is the same
algorithm used by Material Design's tonal palettes.

## Built-in themes

* `Vantage-Dark` (default)
* `Vantage-Light`

Drop new themes under `~/.local/share/vantage/themes/<name>/theme.json`.
List installed themes:

```sh
vantage-theme list
```

## Configuring default theme

```sh
vantage-config set desktop theme Vantage-Dark
vantage-config set desktop dark-mode true
```

Or set both keys in `~/.config/vantage/vantage.conf`:

```ini
[desktop]
theme=Vantage-Dark
dark-mode=true
```

## How it works internally

`vt_theme_apply()` walks the palette and generates:

* GTK3 CSS using `@define-color` directives.
* GTK4 CSS (same syntax).
* Qt5/Qt6 QSS using `QWidget` selectors.
* Sets environment variables so Qt6 apps pick up the platform theme
  through a (future) `libvantage-qt6` plugin — without it, Qt6 still
  picks up the QSS via the `QT_QPA_PLATFORMTHEME=vantage` mechanism.

Vantage does NOT make GTK or Qt6 a dependency of the desktop core —
the integration is one-way (Vantage generates files the toolkits read).
