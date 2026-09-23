/*
 * test-core.c — Unit tests for libvantage-core
 */

#define VT_LOG_DOMAIN "test"
#include <vantage/vt-core.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int _tests_run = 0;
static int _tests_failed = 0;

#define TEST(expr) do { \
    _tests_run++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", #expr, __LINE__); \
        _tests_failed++; \
    } else fprintf(stderr, "."); \
} while (0)

static void test_strings(void) {
    TEST(vt_streq("foo", "foo"));
    TEST(!vt_streq("foo", "bar"));
    TEST(vt_strcaseeq("Foo", "fOO"));
    TEST(vt_strstartswith("hello world", "hello"));
    TEST(!vt_strstartswith("hello world", "world"));
    TEST(vt_strendswith("hello.txt", ".txt"));
    TEST(!vt_strendswith("hello.txt", ".md"));
    char *r = vt_strreplace("hello world hello", "hello", "hi");
    TEST(vt_streq(r, "hi world hi"));
    vt_free(r);

    char *j = vt_strprintf("%d/%d", 1, 2);
    TEST(vt_streq(j, "1/2"));
    vt_free(j);
}

static void test_integers(void) {
    long v;
    TEST(vt_parse_int("12345", &v) && v == 12345);
    TEST(vt_parse_int("-42", &v) && v == -42);
    TEST(vt_parse_int("0x1f", &v) && v == 0x1f);
    TEST(!vt_parse_int("not a number", &v));
    bool b;
    TEST(vt_parse_bool("true", &b) && b);
    TEST(vt_parse_bool("no", &b) && !b);
    TEST(!vt_parse_bool("maybe", &b));
}

static void test_vec(void) {
    vt_vec_t v;
    TEST(vt_vec_init(&v, sizeof(int), 4));
    int vals[] = {1, 2, 3, 4, 5, 6};
    for (size_t i = 0; i < 6; i++) TEST(vt_vec_push(&v, &vals[i]) != NULL);
    TEST(v.size == 6);
    for (size_t i = 0; i < 6; i++) {
        int *p = vt_vec_at(&v, i);
        TEST(*p == vals[i]);
    }
    vt_vec_remove(&v, 0);
    TEST(v.size == 5);
    int *first = vt_vec_at(&v, 0);
    TEST(*first == 2);
    vt_vec_fini(&v);
}

static void test_hash(void) {
    vt_hash_t *h = vt_hash_new(8);
    TEST(h);
    TEST(vt_hash_put(h, "foo", (void *)1));
    TEST(vt_hash_put(h, "bar", (void *)2));
    TEST(vt_hash_size(h) == 2);
    TEST((long)vt_hash_get(h, "foo") == 1);
    TEST((long)vt_hash_get(h, "bar") == 2);
    TEST(vt_hash_get(h, "nope") == NULL);
    TEST(vt_hash_del(h, "foo"));
    TEST(vt_hash_size(h) == 1);
    vt_hash_free(h);
}

static void test_list(void) {
    vt_list_t l; vt_list_init(&l);
    int vals[] = {10, 20, 30};
    vt_list_push_back(&l, &vals[0]);
    vt_list_push_back(&l, &vals[1]);
    vt_list_push_front(&l, &vals[2]);
    TEST(vt_list_len(&l) == 3);
    void *p = vt_list_pop_front(&l);
    TEST(*(int *)p == 30);
    p = vt_list_pop_back(&l);
    TEST(*(int *)p == 20);
    vt_list_fini(&l, NULL);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vt_log_set_level(VT_LOG_OFF);
    test_strings();
    test_integers();
    test_vec();
    test_hash();
    test_list();
    fprintf(stderr, "\n%d tests run, %d failed\n", _tests_run, _tests_failed);
    return _tests_failed ? 1 : 0;
}
