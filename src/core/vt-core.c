/*
 * vt-core.c — Vantage core library implementation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides logging, memory, strings, integer parsing, paths, time, RNG.
 * Uses only libc/libpthread. No external deps.
 */

#define VT_LOG_DOMAIN "core"
#include <vantage/vt-core.h>

#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

/* ===================================================================== LOG */
static const char *const _log_names[] = {
    "TRACE", "DEBUG", "INFO", "NOTICE",
    "WARN", "ERROR", "CRIT", "OFF",
};

static struct {
    pthread_mutex_t lock;
    vt_log_level_t  level;
    FILE           *sink;
} _log = {
    .lock  = PTHREAD_MUTEX_INITIALIZER,
    .level = VT_LOG_INFO,
    .sink  = NULL,
};

void vt_log_set_level(vt_log_level_t lvl) { _log.level = lvl; }
vt_log_level_t vt_log_get_level(void)     { return _log.level; }

void vt_log_set_sink(FILE *fp) {
    pthread_mutex_lock(&_log.lock);
    _log.sink = fp;
    pthread_mutex_unlock(&_log.lock);
}

void vt_vlog(vt_log_level_t lvl, const char *domain,
              const char *file, int line, const char *fmt, va_list ap)
{
    if (lvl < _log.level || lvl >= VT_LOG_OFF) return;
    pthread_mutex_lock(&_log.lock);
    FILE *fp = _log.sink ? _log.sink : stderr;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s %s %s:%d: ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000,
            domain ? domain : "vantage",
            _log_names[lvl], file ? file : "?", line);
    vfprintf(fp, fmt, ap);
    fputc('\n', fp);
    fflush(fp);
    pthread_mutex_unlock(&_log.lock);
}

void vt_log(vt_log_level_t lvl, const char *domain,
            const char *file, int line, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vt_vlog(lvl, domain, file, line, fmt, ap);
    va_end(ap);
}

const char *vt_strerror(int err) {
    static __thread char buf[64];
    switch (err) {
    case VT_OK:           return "OK";
    case VT_ERR_NOMEM:    return "Out of memory";
    case VT_ERR_INVAL:    return "Invalid argument";
    case VT_ERR_NOENT:    return "No such file or directory";
    case VT_ERR_IO:       return "I/O error";
    case VT_ERR_PERM:     return "Permission denied";
    case VT_ERR_BUSY:     return "Resource busy";
    case VT_ERR_EXIST:    return "Already exists";
    case VT_ERR_RANGE:    return "Out of range";
    case VT_ERR_NOTSUPP:  return "Operation not supported";
    case VT_ERR_TIMEOUT:  return "Timed out";
    case VT_ERR_PROTOCOL: return "Protocol error";
    case VT_ERR_PARTIAL:  return "Partial success";
    case VT_ERR:          return "Error";
    default:              snprintf(buf, sizeof(buf), "errno %d", -err); return buf;
    }
}

/* ============================================================== ALLOCATORS */
void *vt_malloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p && n) {
        vt_loge("OOM requesting %zu bytes", n);
        abort();
    }
    return p;
}
void *vt_malloc0(size_t n) {
    void *p = calloc(n ? n : 1, 1);
    if (!p && n) { vt_loge("OOM allocating %zu", n); abort(); }
    return p;
}
void *vt_calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p = calloc(total ? total : 1, 1);
    if (!p && total) { vt_loge("OOM calloc %zu", total); abort(); }
    return p;
}
void *vt_realloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q && n) { vt_loge("OOM realloc %zu", n); return NULL; }
    return q;
}
void *vt_memdup(const void *src, size_t n) {
    void *p = vt_malloc(n);
    if (p) memcpy(p, src, n);
    return p;
}
char *vt_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = vt_malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}
char *vt_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t m = strnlen(s, n);
    char *r = vt_malloc(m + 1);
    memcpy(r, s, m);
    r[m] = 0;
    return r;
}
void vt_free(void *p) { free(p); }
void vt_freep(void *p) {
    if (!p) return;
    void **pp = (void **)p;
    free(*pp);
    *pp = NULL;
}

/* ============================================================ STRINGS */
size_t vt_strlen(const char *s) { return s ? strlen(s) : 0; }

char *vt_strncpy_safe(char *dst, const char *src, size_t n) {
    if (!dst || n == 0) return dst;
    if (!src) { dst[0] = 0; return dst; }
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

int vt_strcasecmp_ascii(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool vt_streq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}
bool vt_strcaseeq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return vt_strcasecmp_ascii(a, b) == 0;
}
bool vt_strstartswith(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}
bool vt_strendswith(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lp = strlen(suffix);
    if (lp > ls) return false;
    return strcmp(s + (ls - lp), suffix) == 0;
}

char *vt_strtrim(char *s) {
    if (!s) return NULL;
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}
char *vt_strchomp(char *s) { return vt_strtrim(s); }

char **vt_strsplit(const char *s, const char *delims, size_t *out_n) {
    if (!s || !delims) { if (out_n) *out_n = 0; return NULL; }
    size_t cap = 8, n = 0;
    char **arr = vt_malloc(sizeof(char*) * cap);
    const char *p = s;
    while (*p) {
        while (*p && strchr(delims, *p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !strchr(delims, *p)) p++;
        size_t len = (size_t)(p - start);
        if (n + 1 >= cap) { cap *= 2; arr = vt_realloc(arr, sizeof(char*) * cap); }
        arr[n++] = vt_strndup(start, len);
    }
    if (out_n) *out_n = n;
    return arr;
}

char **vt_strsplit_lines(const char *s, size_t *out_n) {
    return vt_strsplit(s, "\r\n", out_n);
}

char *vt_strjoin(char **parts, size_t n, const char *sep) {
    if (!parts || n == 0) return vt_strdup("");
    if (!sep) sep = "";
    size_t seplen = strlen(sep);
    size_t total = 1;
    for (size_t i = 0; i < n; i++) total += (parts[i] ? strlen(parts[i]) : 0);
    total += (n - 1) * seplen;
    char *buf = vt_malloc(total);
    char *w = buf;
    for (size_t i = 0; i < n; i++) {
        if (i) { memcpy(w, sep, seplen); w += seplen; }
        if (parts[i]) { size_t l = strlen(parts[i]); memcpy(w, parts[i], l); w += l; }
    }
    *w = 0;
    return buf;
}

char *vt_vstrprintf(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return NULL;
    char *r = vt_malloc((size_t)n + 1);
    vsnprintf(r, (size_t)n + 1, fmt, ap);
    return r;
}

char *vt_strprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *r = vt_vstrprintf(fmt, ap);
    va_end(ap);
    return r;
}

char *vt_strreplace(const char *src, const char *needle, const char *with) {
    if (!src) return NULL;
    if (!needle || !*needle) return vt_strdup(src);
    if (!with) with = "";
    size_t nlen = strlen(needle), wlen = strlen(with);
    size_t srclen = strlen(src);
    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, needle))) { count++; p += nlen; }
    size_t out_len = srclen + count * (wlen - nlen) + 1;
    char *out = vt_malloc(out_len);
    char *o = out;
    p = src;
    const char *prev = src;
    while ((p = strstr(p, needle))) {
        size_t pre = (size_t)(p - prev);
        memcpy(o, prev, pre); o += pre;
        memcpy(o, with, wlen); o += wlen;
        p += nlen; prev = p;
    }
    size_t tail = strlen(prev);
    memcpy(o, prev, tail); o += tail;
    *o = 0;
    return out;
}

char *vt_strescape(const char *src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char *out = vt_malloc(n * 4 + 1);
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == '\\' || c == '"') { *o++ = '\\'; *o++ = c; }
        else if (c == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (c == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (c == '\t') { *o++ = '\\'; *o++ = 't'; }
        else if ((unsigned char)c < 0x20) {
            o += snprintf(o, 5, "\\x%02x", (unsigned char)c);
        } else *o++ = c;
    }
    *o = 0;
    return out;
}

void vt_strv_free(char **v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) vt_free(v[i]);
    vt_free(v);
}

/* ============================================================ INTEGERS */
bool vt_parse_int(const char *s, long *out) {
    if (!s || !*s) return false;
    char *end; errno = 0;
    long v = strtol(s, &end, 0);
    if (errno || *end) return false;
    if (out) *out = v;
    return true;
}
bool vt_parse_uint(const char *s, unsigned long *out) {
    if (!s || !*s) return false;
    char *end; errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno || *end) return false;
    if (out) *out = v;
    return true;
}
bool vt_parse_double(const char *s, double *out) {
    if (!s || !*s) return false;
    char *end; errno = 0;
    double v = strtod(s, &end);
    if (errno || *end) return false;
    if (out) *out = v;
    return true;
}
bool vt_parse_bool(const char *s, bool *out) {
    if (!s) return false;
    if (vt_strcaseeq(s, "1") || vt_strcaseeq(s, "true") ||
        vt_strcaseeq(s, "yes") || vt_strcaseeq(s, "on")) {
        if (out) *out = true;
        return true;
    }
    if (vt_strcaseeq(s, "0") || vt_strcaseeq(s, "false") ||
        vt_strcaseeq(s, "no") || vt_strcaseeq(s, "off")) {
        if (out) *out = false;
        return true;
    }
    return false;
}
const char *vt_bool_str(bool b) { return b ? "true" : "false"; }
char *vt_int_to_str(long v) { return vt_strprintf("%ld", v); }

/* ============================================================ PATHS */
const char *vt_home_dir(void) {
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/";
}
const char *vt_config_dir(void) {
    const char *x = getenv("XDG_CONFIG_HOME");
    static char buf[256];
    if (x && *x) return x;
    snprintf(buf, sizeof(buf), "%s/.config", vt_home_dir());
    return buf;
}
const char *vt_cache_dir(void) {
    const char *x = getenv("XDG_CACHE_HOME");
    static char buf[256];
    if (x && *x) return x;
    snprintf(buf, sizeof(buf), "%s/.cache", vt_home_dir());
    return buf;
}
const char *vt_data_dir_user(void) {
    const char *x = getenv("XDG_DATA_HOME");
    static char buf[256];
    if (x && *x) return x;
    snprintf(buf, sizeof(buf), "%s/.local/share", vt_home_dir());
    return buf;
}
const char *vt_runtime_dir(void) {
    const char *x = getenv("XDG_RUNTIME_DIR");
    static char buf[256];
    if (x && *x) return x;
    snprintf(buf, sizeof(buf), "/run/user/%d", (int)geteuid());
    return buf;
}

char *vt_path_join(const char *a, const char *b) {
    if (!a || !*a) return vt_strdup(b ? b : "");
    if (!b || !*b) return vt_strdup(a);
    size_t la = strlen(a), lb = strlen(b);
    char *r = vt_malloc(la + lb + 2);
    memcpy(r, a, la);
    if (r[la - 1] != '/') r[la++] = '/';
    memcpy(r + la, b, lb + 1);
    return r;
}

char *vt_path_join_many(const char *first, ...) {
    if (!first) return NULL;
    char *acc = vt_strdup(first);
    va_list ap; va_start(ap, first);
    const char *p;
    while ((p = va_arg(ap, const char*))) {
        char *t = vt_path_join(acc, p);
        vt_free(acc);
        acc = t;
    }
    va_end(ap);
    return acc;
}

bool vt_path_exists(const char *p) { return p && access(p, F_OK) == 0; }
bool vt_path_is_dir(const char *p) {
    if (!p) return false;
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
bool vt_path_is_file(const char *p) {
    if (!p) return false;
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

char *vt_path_expand(const char *p) {
    if (!p) return NULL;
    if (*p == '~') {
        const char *rest = p + 1;
        if (*rest == '/' || *rest == 0)
            return vt_path_join(vt_home_dir(), rest);
    }
    if (strchr(p, '$')) {
        /* tiny $VAR expander */
        size_t cap = strlen(p) + 256;
        char *out = vt_malloc(cap);
        const char *src = p;
        char *o = out;
        while (*src && (size_t)(o - out) < cap - 1) {
            if (*src == '$') {
                src++;
                char var[64]; size_t vn = 0;
                if (*src == '{') { src++; while (*src && *src != '}' && vn < sizeof(var)-1) var[vn++] = *src++; if (*src == '}') src++; }
                else { while (*src && (isalnum((unsigned char)*src) || *src == '_') && vn < sizeof(var)-1) var[vn++] = *src++; }
                var[vn] = 0;
                const char *val = getenv(var);
                if (val) {
                    size_t lv = strlen(val);
                    while ((size_t)(o - out) + lv + 1 < cap) { *o++ = *val++; val++; if(!*val) break; }
                }
            } else *o++ = *src++;
        }
        *o = 0;
        return out;
    }
    return vt_strdup(p);
}

/* ============================================================ VECTORS */
bool vt_vec_init(vt_vec_t *v, size_t elem_sz, size_t cap) {
    if (!v || !elem_sz) return false;
    v->elem_sz = elem_sz; v->size = 0; v->cap = cap;
    v->data = v->cap ? vt_malloc0(v->cap * v->elem_sz) : NULL;
    return true;
}
void vt_vec_clear(vt_vec_t *v) { if (v) v->size = 0; }
void vt_vec_fini(vt_vec_t *v) { if (!v) return; vt_free(v->data); v->data = NULL; v->size = v->cap = 0; }
void *vt_vec_push(vt_vec_t *v, const void *elem) {
    if (!v) return NULL;
    if (v->size + 1 > v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 8;
        void *nd = vt_realloc(v->data, nc * v->elem_sz);
        if (!nd) return NULL;
        v->data = nd; v->cap = nc;
    }
    void *p = (char *)v->data + v->size * v->elem_sz;
    if (elem) memcpy(p, elem, v->elem_sz);
    v->size++;
    return p;
}
void *vt_vec_push_zero(vt_vec_t *v) {
    void *p = vt_vec_push(v, NULL);
    if (p) memset(p, 0, v->elem_sz);
    return p;
}
void *vt_vec_at(const vt_vec_t *v, size_t i) {
    if (!v || i >= v->size) return NULL;
    return (char *)v->data + i * v->elem_sz;
}
void vt_vec_pop(vt_vec_t *v) {
    if (v && v->size) v->size--;
}
void vt_vec_remove(vt_vec_t *v, size_t i) {
    if (!v || i >= v->size) return;
    if (i + 1 < v->size)
        memmove((char *)v->data + i * v->elem_sz,
                (char *)v->data + (i + 1) * v->elem_sz,
                (v->size - i - 1) * v->elem_sz);
    v->size--;
}
void *vt_vec_find(const vt_vec_t *v, const void *key,
                  int (*cmp)(const void *, const void *)) {
    if (!v || !cmp) return NULL;
    for (size_t i = 0; i < v->size; i++) {
        void *p = (char *)v->data + i * v->elem_sz;
        if (cmp(p, key) == 0) return p;
    }
    return NULL;
}
void vt_vec_sort(vt_vec_t *v, int (*cmp)(const void *, const void *)) {
    if (!v || !cmp || v->size < 2) return;
    /* tiny insertion sort — stable, simple, small N typical */
    for (size_t i = 1; i < v->size; i++) {
        char tmp[v->elem_sz];
        memcpy(tmp, (char *)v->data + i * v->elem_sz, v->elem_sz);
        ssize_t j = (ssize_t)i - 1;
        while (j >= 0 && cmp((char *)v->data + j * v->elem_sz, tmp) > 0) {
            memcpy((char *)v->data + (j + 1) * v->elem_sz,
                   (char *)v->data + j * v->elem_sz, v->elem_sz);
            j--;
        }
        memcpy((char *)v->data + (j + 1) * v->elem_sz, tmp, v->elem_sz);
    }
}

/* ============================================================ LIST */
void vt_list_init(vt_list_t *l) {
    if (!l) return;
    l->head.prev = l->head.next = &l->head;
    l->n = 0;
}
void vt_list_fini(vt_list_t *l, void (*free_fn)(void *)) {
    if (!l) return;
    vt_node_t *cur = l->head.next;
    while (cur && cur != &l->head) {
        vt_node_t *next = cur->next;
        if (free_fn) free_fn(cur->data);
        vt_free(cur);
        cur = next;
    }
    l->head.prev = l->head.next = &l->head;
    l->n = 0;
}
vt_node_t *vt_list_push_back(vt_list_t *l, void *data) {
    if (!l) return NULL;
    vt_node_t *n = vt_malloc0(sizeof(*n));
    n->data = data;
    n->next = &l->head; n->prev = l->head.prev;
    l->head.prev->next = n; l->head.prev = n;
    l->n++;
    return n;
}
vt_node_t *vt_list_push_front(vt_list_t *l, void *data) {
    if (!l) return NULL;
    vt_node_t *n = vt_malloc0(sizeof(*n));
    n->data = data;
    n->prev = &l->head; n->next = l->head.next;
    l->head.next->prev = n; l->head.next = n;
    l->n++;
    return n;
}
void vt_list_remove(vt_list_t *l, vt_node_t *n) {
    if (!l || !n) return;
    n->prev->next = n->next;
    n->next->prev = n->prev;
    vt_free(n);
    l->n--;
}
void *vt_list_pop_front(vt_list_t *l) {
    if (!l || l->n == 0) return NULL;
    vt_node_t *n = l->head.next;
    void *d = n->data;
    vt_list_remove(l, n);
    return d;
}
void *vt_list_pop_back(vt_list_t *l) {
    if (!l || l->n == 0) return NULL;
    vt_node_t *n = l->head.prev;
    void *d = n->data;
    vt_list_remove(l, n);
    return d;
}
size_t vt_list_len(const vt_list_t *l) { return l ? l->n : 0; }
void vt_list_foreach(vt_list_t *l, void (*fn)(void *, void *), void *ud) {
    if (!l || !fn) return;
    vt_node_t *cur = l->head.next;
    while (cur && cur != &l->head) {
        vt_node_t *next = cur->next;
        fn(cur->data, ud);
        cur = next;
    }
}

/* ============================================================ HASH */
typedef struct _hash_node _hash_node_t;
struct _hash_node {
    _hash_node_t *next;
    char  *key;
    void  *val;
    bool   owned;
};

struct vt_hash {
    _hash_node_t **buckets;
    size_t         nbuckets;
    size_t         size;
};

static uint64_t _hash_fnv(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x100000001b3ULL; }
    return h;
}

vt_hash_t *vt_hash_new(size_t init_buckets) {
    vt_hash_t *h = vt_malloc0(sizeof(*h));
    h->nbuckets = init_buckets ? init_buckets : 16;
    h->buckets = vt_malloc0(h->nbuckets * sizeof(_hash_node_t*));
    return h;
}

void vt_hash_free(vt_hash_t *h) {
    if (!h) return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        _hash_node_t *cur = h->buckets[i];
        while (cur) {
            _hash_node_t *next = cur->next;
            vt_free(cur->key);
            vt_free(cur);
            cur = next;
        }
    }
    vt_free(h->buckets);
    vt_free(h);
}
void vt_hash_free_full(vt_hash_t *h, void (*v_free)(void *)) {
    if (!h) return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        _hash_node_t *cur = h->buckets[i];
        while (cur) {
            _hash_node_t *next = cur->next;
            if (v_free) v_free(cur->val);
            vt_free(cur->key);
            vt_free(cur);
            cur = next;
        }
    }
    vt_free(h->buckets);
    vt_free(h);
}
static void _hash_rehash(vt_hash_t *h, size_t nb) {
    _hash_node_t **nbuckets = vt_malloc0(nb * sizeof(_hash_node_t*));
    for (size_t i = 0; i < h->nbuckets; i++) {
        _hash_node_t *cur = h->buckets[i];
        while (cur) {
            _hash_node_t *next = cur->next;
            uint64_t k = _hash_fnv(cur->key) % nb;
            cur->next = nbuckets[k]; nbuckets[k] = cur;
            cur = next;
        }
    }
    vt_free(h->buckets);
    h->buckets = nbuckets;
    h->nbuckets = nb;
}
void *vt_hash_get(vt_hash_t *h, const char *key) {
    if (!h || !key) return NULL;
    uint64_t k = _hash_fnv(key) % h->nbuckets;
    _hash_node_t *cur = h->buckets[k];
    while (cur) {
        if (strcmp(cur->key, key) == 0) return cur->val;
        cur = cur->next;
    }
    return NULL;
}
bool vt_hash_put(vt_hash_t *h, const char *key, void *val) {
    return vt_hash_put_owned(h, key, val);
}
bool vt_hash_put_owned(vt_hash_t *h, const char *key, void *val) {
    if (!h || !key) return false;
    uint64_t k = _hash_fnv(key) % h->nbuckets;
    _hash_node_t *cur = h->buckets[k];
    while (cur) {
        if (strcmp(cur->key, key) == 0) { cur->val = val; return true; }
        cur = cur->next;
    }
    _hash_node_t *n = vt_malloc0(sizeof(*n));
    n->key = vt_strdup(key);
    n->val = val;
    n->next = h->buckets[k]; h->buckets[k] = n;
    h->size++;
    if (h->size > h->nbuckets * 2)
        _hash_rehash(h, h->nbuckets * 2);
    return true;
}
bool vt_hash_del(vt_hash_t *h, const char *key) {
    if (!h || !key) return false;
    uint64_t k = _hash_fnv(key) % h->nbuckets;
    _hash_node_t *cur = h->buckets[k], *prev = NULL;
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            if (prev) prev->next = cur->next;
            else h->buckets[k] = cur->next;
            vt_free(cur->key);
            vt_free(cur);
            h->size--;
            return true;
        }
        prev = cur; cur = cur->next;
    }
    return false;
}
size_t vt_hash_size(const vt_hash_t *h) { return h ? h->size : 0; }

vt_hash_iter_t vt_hash_iter_begin(vt_hash_t *h) {
    vt_hash_iter_t it = { h, 0, NULL };
    for (size_t i = 0; i < h->nbuckets; i++) {
        if (h->buckets[i]) { it.idx = i; it.cur = h->buckets[i]; break; }
    }
    return it;
}
bool vt_hash_iter_next(vt_hash_iter_t *it, const char **key, void **val) {
    if (!it || !it->h) return false;
    _hash_node_t *cur = it->cur;
    if (!cur) {
        for (size_t i = it->idx; i < it->h->nbuckets; i++) {
            if (it->h->buckets[i]) { cur = it->h->buckets[i]; it->idx = i; break; }
        }
        if (!cur) return false;
    }
    if (key) *key = cur->key;
    if (val) *val = cur->val;
    it->cur = cur->next;
    if (!it->cur) it->idx++;
    return true;
}

/* ============================================================ FILE */
char *vt_file_read_all(const char *path, size_t *out_len) {
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = vt_malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = 0;
    if (out_len) *out_len = rd;
    return buf;
}
bool vt_file_write_all(const char *path, const void *data, size_t len) {
    if (!path || (!data && len)) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t w = fwrite(data ? data : "", 1, len, fp);
    fclose(fp);
    return w == len;
}
char *vt_file_read_line(FILE *fp) {
    if (!fp) return NULL;
    size_t cap = 128, n = 0;
    char *buf = vt_malloc(cap);
    int c;
    while ((c = fgetc(fp)) != EOF && c != '\n') {
        if (n + 1 >= cap) { cap *= 2; buf = vt_realloc(buf, cap); }
        buf[n++] = (char)c;
    }
    if (c == EOF && n == 0) { vt_free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

bool vt_file_mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return false;
    char *tmp = vt_strdup(path);
    char *p = tmp;
    if (*p == '/') p++;
    while (*p) {
        if (*p == '/') { *p = 0; mkdir(tmp, mode); *p = '/'; }
        p++;
    }
    bool ok = (mkdir(tmp, mode) == 0) || (errno == EEXIST);
    vt_free(tmp);
    return ok;
}
bool vt_file_exists(const char *p) { return vt_path_exists(p); }
bool vt_file_is_dir(const char *p) { return vt_path_is_dir(p); }

char **vt_file_list_dir(const char *path, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!path) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;
    size_t cap = 16, n = 0;
    char **arr = vt_malloc(sizeof(char*) * cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
            (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        if (n + 1 >= cap) { cap *= 2; arr = vt_realloc(arr, sizeof(char*) * cap); }
        arr[n++] = vt_strdup(e->d_name);
    }
    closedir(d);
    if (out_n) *out_n = n;
    return arr;
}

char *vt_file_basename(const char *path) {
    if (!path) return NULL;
    const char *p = strrchr(path, '/');
    return vt_strdup(p ? p + 1 : path);
}
char *vt_file_dirname(const char *path) {
    if (!path) return NULL;
    const char *p = strrchr(path, '/');
    if (!p) return vt_strdup(".");
    if (p == path) return vt_strdup("/");
    return vt_strndup(path, (size_t)(p - path));
}

/* ============================================================ TIME */
uint64_t vt_time_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint64_t vt_time_now_ms(void) { return vt_time_now_us() / 1000; }
uint64_t vt_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void vt_time_sleep_ms(uint32_t ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
void vt_time_format_iso(uint64_t ms, char *buf, size_t n) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03u",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (unsigned)(ms % 1000));
}

/* ============================================================ RNG */
static __thread uint64_t _rng_state = 0x9E3779B97F4A7C15ULL;
static void _rng_init(void) {
    if (_rng_state == 0x9E3779B97F4A7C15ULL) {
        _rng_state = vt_time_now_ns() ^ (uint64_t)pthread_self();
        if (_rng_state == 0) _rng_state = 1;
    }
}
uint64_t vt_rng_next(void) {
    _rng_init();
    uint64_t x = _rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    _rng_state = x;
    return x;
}
void vt_rng_seed(uint64_t s) {
    _rng_state = s ? s : 0xDEADBEEFCAFEBABEULL;
}
double vt_rng_uniform(void) {
    return (double)(vt_rng_next() >> 11) / 9007199254740992.0;
}

/* ============================================================ SEARCH */
void *vt_mem_search(const void *hay, size_t hay_n,
                     const void *needle, size_t needle_sz) {
    if (!hay || !needle || needle_sz == 0 || hay_n < needle_sz) return NULL;
    const char *h = hay;
    const char *n = needle;
    size_t end = hay_n - needle_sz;
    for (size_t i = 0; i <= end; i++) {
        size_t j = 0;
        for (; j < needle_sz; j++) if (h[i + j] != n[j]) break;
        if (j == needle_sz) return (void *)(h + i);
    }
    return NULL;
}
