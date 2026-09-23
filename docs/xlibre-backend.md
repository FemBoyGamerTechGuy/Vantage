# Vantage XLibre Backend

[XLibre](https://github.com/XLibre/XLibre) is a fork of Xorg that fixes
long-standing issues without breaking API compatibility. Vantage treats
XLibre as a first-class X11 backend.

## Detection

Vantage probes for XLibre by:

1. Checking the `VANTAGE_XLIBRE=1` environment variable.
2. (Future) Querying the X server name via `Xserver` properties.
3. Falling back to the standard X11 code path (which works for both
   Xorg and XLibre since they share libxcb/libX11).

## Wire compatibility

XLibre is API-compatible with Xorg at the libxcb level. From Vantage's
point of view, the same backend code path is used — the `kind` field
in `vt_backend_t` is just set to `VT_BACKEND_XLIBRE` so the diagnostics
tool can show the correct name.

## Selection

```ini
[desktop]
backend=xlibre
```

Or rely on auto-detection: if both `DISPLAY` is set and XLibre is
detected, Vantage will use the XLibre backend. Otherwise, it falls back
to the Xorg backend, then Wayland, then headless.

## Why this matters

Vantage supports XLibre because:

1. It is increasingly used by users who want bug-fixes faster than
   Xorg's release cadence.
2. XLibre maintains ABI compat, so existing X11 applications work.
3. Vantage's design goal is "no assumptions that only work with Xorg".
