/* registry-probe.c — connect, dump globals, verify the staging
 * protocols are advertised (independent of any toolkit's own bugs).
 * Exit code 0 = all five expected globals seen. */
#include <stdio.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

static int have_cshape = 0, have_act = 0, have_fscale = 0,
           have_ticon = 0, have_viewporter = 0, have_shm = 0;

static void registry_global(void *data, struct wl_registry *r,
                            uint32_t name, const char *iface,
                            uint32_t version) {
    (void)data;
    if (strcmp(iface, "wp_cursor_shape_manager_v1") == 0)
        have_cshape = 1;
    else if (strcmp(iface, "xdg_activation_v1") == 0)
        have_act = 1;
    else if (strcmp(iface, "wp_fractional_scale_manager_v1") == 0)
        have_fscale = 1;
    else if (strcmp(iface, "xdg_toplevel_icon_manager_v1") == 0)
        have_ticon = 1;
    else if (strcmp(iface, "wp_viewporter") == 0)
        have_viewporter = 1;
    else if (strcmp(iface, "wl_shm") == 0)
        have_shm = 1;
    printf("global %5u v%u %s\n", name, version, iface);
}

static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name) {
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener reg_l = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "connect failed\n"); return 2; }
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &reg_l, NULL);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    printf("cursor-shape=%d activation=%d fractional=%d toplevel-icon=%d "
           "viewporter=%d shm=%d\n",
           have_cshape, have_act, have_fscale, have_ticon,
           have_viewporter, have_shm);
    wl_registry_destroy(r);
    wl_display_disconnect(d);
    return (have_cshape && have_act && have_fscale && have_ticon &&
            have_viewporter) ? 0 : 1;
}
