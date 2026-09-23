/*
 * test-ipc.c — vt_ipc_t encode/decode tests
 */

#define VT_LOG_DOMAIN "test"
#include <vantage/vt-ipc.h>
#include <vantage/vt-core.h>
#include <stdio.h>
#include <string.h>

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

    vt_ipc_msg_t m = { .id = 0x1234, .type = VT_IPC_MSG_REQUEST, .len = 5 };
    m.payload = (uint8_t *)"hello";
    uint8_t *buf; size_t n;
    TEST(vt_ipc_encode(&m, &buf, &n) == VT_IPC_OK);
    TEST(n == 16 + 5);
    uint32_t magic;
    memcpy(&magic, buf, 4);
    TEST(magic == VT_IPC_MAGIC);
    vt_ipc_msg_t m2;
    TEST(vt_ipc_decode(buf, n, &m2) == VT_IPC_OK);
    TEST(m2.id == m.id);
    TEST(m2.type == m.type);
    TEST(m2.len == m.len);
    TEST(memcmp(m2.payload, m.payload, m.len) == 0);
    vt_free(m2.payload);
    vt_free(buf);

    fprintf(stderr, "\n%d tests run, %d failed\n", _tests_run, _tests_failed);
    return _tests_failed ? 1 : 0;
}
