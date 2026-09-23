/*
 * test-config.c — vt_config_t parser tests
 */

#define VT_LOG_DOMAIN "test"
#include <vantage/vt-config.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST(expr) do { \
    _tests_run++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", #expr, __LINE__); \
        _tests_failed++; \
    } else fprintf(stderr, "."); \
} while (0)

static int _tests_run = 0;
static int _tests_failed = 0;

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vt_log_set_level(VT_LOG_OFF);

    vt_config_t *c = vt_config_new();
    TEST(c);
    /* Parse a string with comments, sections, line continuations */
    const char *data =
        "# header comment\n"
        "[desktop]\n"
        "theme = Vantage-Dark\n"
        "dark-mode = true\n"
        "[panel]\n"
        "height = 32\n"
        "position = top\n";
    /* write to temp file */
    FILE *fp = fopen("/tmp/_vantage_test.conf", "w");
    fputs(data, fp);
    fclose(fp);
    TEST(vt_config_load(c, "/tmp/_vantage_test.conf") == VT_OK);
    TEST(vt_streq(vt_config_get(c, "desktop", "theme", ""), "Vantage-Dark"));
    TEST(vt_config_get_bool(c, "desktop", "dark-mode", false));
    TEST(vt_config_get_int(c, "panel", "height", 0) == 32);
    TEST(vt_streq(vt_config_get(c, "panel", "position", ""), "top"));
    TEST(vt_streq(vt_config_get(c, "desktop", "nonexistent", "fallback"), "fallback"));
    /* set + save + reload */
    vt_config_set(c, "panel", "height", "48");
    TEST(vt_config_save(c, "/tmp/_vantage_test2.conf") == VT_OK);
    vt_config_t *c2 = vt_config_new();
    vt_config_load(c2, "/tmp/_vantage_test2.conf");
    TEST(vt_config_get_int(c2, "panel", "height", 0) == 48);
    vt_config_free(c2);
    vt_config_free(c);
    unlink("/tmp/_vantage_test.conf");
    unlink("/tmp/_vantage_test2.conf");
    fprintf(stderr, "\n%d tests run, %d failed\n", _tests_run, _tests_failed);
    return _tests_failed ? 1 : 0;
}
