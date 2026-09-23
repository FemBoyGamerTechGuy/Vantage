# Vantage Plugin API

Vantage plugins are small C modules that implement the
`vt_panel_plugin_api_t` interface. They can be either built-in
(linked into the panel binary) or runtime-loaded via `dlopen()`.

## ABI

```c
typedef struct vt_panel_plugin_api {
    void  *(*init)(vt_panel_t *p);
    void   (*fini)(void *inst);
    void   (*render)(void *inst, vt_renderer_t *r, vt_rect_t area);
    void   (*on_event)(void *inst, int ev, void *ev_data);
    size_t (*preferred_size)(void *inst);
    const char *name;
    int    api_version;
} vt_panel_plugin_api_t;
```

| Field              | Required? | Purpose                          |
|--------------------|-----------|----------------------------------|
| `init`             | yes       | Allocate plugin state             |
| `fini`             | yes       | Free plugin state                 |
| `render`           | yes       | Draw into the panel area          |
| `on_event`        | no        | Click/key events                   |
| `preferred_size`  | yes       | Width/height requested            |
| `name`             | yes       | Identifier for debugging          |
| `api_version`      | yes       | Use 1 for current ABI             |

## Example plugin

```c
#define VT_LOG_DOMAIN "hello"
#include <vantage/vt-panel.h>
#include <vantage/vt-renderer.h>

static void *hello_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void  hello_fini(void *inst)   { (void)inst; }
static void  hello_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_color_t c = {0.5f, 0.5f, 0.5f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
}
static size_t hello_size(void *inst) { (void)inst; return 60; }

const vt_panel_plugin_api_t vantage_plugin_hello = {
    .init = hello_init,
    .fini = hello_fini,
    .render = hello_render,
    .preferred_size = hello_size,
    .name = "hello",
    .api_version = 1,
};
```

## Registering a plugin

### Built-in

Link your plugin C file into the panel binary, then call:

```c
vt_panel_applet_register(&vantage_plugin_hello);
```

at startup.

### Runtime-loaded

Compile your plugin as a shared object:

```sh
cc -shared -fPIC -I/path/to/vantage-headers \
   -o libvantage-plugin-hello.so hello.c
```

Drop it in `/usr/lib/vantage/plugins/`. The panel loads `.so` files
in that directory at startup (TODO — dlopen loader is a future work
item).

## Events

`on_event` receives an event code (`vt_input_event_kind_t`) and the
event data. The most common for panel plugins are
`VT_INPUT_BUTTON_PRESS`, `VT_INPUT_MOTION` (pointer enter/leave).

## Applet kinds

Built-in applet kinds:

| Constant                       | Description                          |
|--------------------------------|--------------------------------------|
| `VT_PANEL_APPLET_LAUNCHER`    | Application menu / launcher           |
| `VT_PANEL_APPLET_TASKLIST`    | Open window tasklist                  |
| `VT_PANEL_APPLET_CLOCK`       | Clock                                 |
| `VT_PANEL_APPLET_WORKSPACES`  | Workspace switcher                    |
| `VT_PANEL_APPLET_TRAY`        | XEmbed system tray                    |
| `VT_PANEL_APPLET_VOLUME`      | Audio volume                          |
| `VT_PANEL_APPLET_NETWORK`     | Network status                        |
| `VT_PANEL_APPLET_BATTERY`     | Battery indicator                     |
| `VT_PANEL_APPLET_USER`        | User-defined                          |
