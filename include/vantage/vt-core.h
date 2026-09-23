/*
 * vt-core.h — Vantage core utility library (public API)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libvantage-core provides foundational utilities for every other Vantage
 * component: memory, logging, strings, lists, hash tables, dynamic arrays,
 * file I/O, time, RNG. Runtime-depends on libc + libm + libpthread only.
 */
#ifndef VANTAGE_CORE_H
#define VANTAGE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VT_VERSION_STRING VT_VERSION

#define VT_UNUSED(x)        (void)(x)
#define VT_ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define VT_STRINGIFY1(x)   #x
#define VT_STRINGIFY(x)    VT_STRINGIFY1(x)
#define VT_CONCAT1(a,b)    a##b
#define VT_CONCAT(a,b)     VT_CONCAT1(a,b)

/* --------------------------------------------------------- logging */
typedef enum {
    VT_LOG_TRACE = 0, VT_LOG_DEBUG, VT_LOG_INFO, VT_LOG_NOTICE,
    VT_LOG_WARN, VT_LOG_ERROR, VT_LOG_CRITICAL, VT_LOG_OFF,
} vt_log_level_t;

void            vt_log_set_level(vt_log_level_t lvl);
vt_log_level_t  vt_log_get_level(void);
void            vt_log_set_sink(FILE *fp);
void            vt_log(vt_log_level_t lvl, const char *domain,
                       const char *file, int line, const char *fmt, ...)
                       __attribute__((format(printf, 5, 6)));
void            vt_vlog(vt_log_level_t lvl, const char *domain,
                        const char *file, int line, const char *fmt,
                        va_list ap);

#define vt_logt(...)  vt_log(VT_LOG_TRACE,    VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)
#define vt_logd(...)  vt_log(VT_LOG_DEBUG,    VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)
#define vt_logi(...)  vt_log(VT_LOG_INFO,     VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)
#define vt_logn(...)  vt_log(VT_LOG_NOTICE,   VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)
#define vt_logw(...)  vt_log(VT_LOG_WARN,     VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)
#define vt_loge(...)  vt_log(VT_LOG_ERROR,    VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)
#define vt_logc(...)  vt_log(VT_LOG_CRITICAL, VT_LOG_DOMAIN, __FILE__, __LINE__, __VA_ARGS__)

#ifndef VT_LOG_DOMAIN
#define VT_LOG_DOMAIN "vantage"
#endif

/* --------------------------------------------------- error codes */
typedef enum {
    VT_OK             =  0,
    VT_ERR            = -1,
    VT_ERR_NOMEM      = -2,
    VT_ERR_INVAL      = -3,
    VT_ERR_NOENT      = -4,
    VT_ERR_IO         = -5,
    VT_ERR_PERM       = -6,
    VT_ERR_BUSY       = -7,
    VT_ERR_EXIST      = -8,
    VT_ERR_RANGE      = -9,
    VT_ERR_NOTSUPP    = -10,
    VT_ERR_TIMEOUT    = -11,
    VT_ERR_PROTOCOL   = -12,
    VT_ERR_PARTIAL    = -13,
} vt_err_t;

const char *vt_strerror(int err);

/* --------------------------------------------------- allocators */
void *vt_malloc(size_t n) __attribute__((malloc));
void *vt_malloc0(size_t n) __attribute__((malloc));
void *vt_calloc(size_t n, size_t sz) __attribute__((malloc));
void *vt_realloc(void *p, size_t n);
void *vt_memdup(const void *src, size_t n);
char *vt_strdup(const char *s) __attribute__((malloc));
char *vt_strndup(const char *s, size_t n) __attribute__((malloc));
void  vt_free(void *p);
void  vt_freep(void *p);

#define VT_AUTO_FREE __attribute__((cleanup(vt_freep)))

/* --------------------------------------------------- string utils */
size_t   vt_strlen(const char *s);
char    *vt_strncpy_safe(char *dst, const char *src, size_t n);
int      vt_strcasecmp_ascii(const char *a, const char *b);
bool     vt_streq(const char *a, const char *b);
bool     vt_strcaseeq(const char *a, const char *b);
bool     vt_strstartswith(const char *s, const char *prefix);
bool     vt_strendswith(const char *s, const char *suffix);
char    *vt_strtrim(char *s);
char    *vt_strchomp(char *s);
char   **vt_strsplit(const char *s, const char *delims, size_t *out_n);
char   **vt_strsplit_lines(const char *s, size_t *out_n);
char    *vt_strjoin(char **parts, size_t n, const char *sep);
char    *vt_strprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char    *vt_vstrprintf(const char *fmt, va_list ap);
char    *vt_strreplace(const char *src, const char *needle, const char *with);
char    *vt_strescape(const char *src);
void     vt_strv_free(char **v, size_t n);

/* --------------------------------------------------- integers */
bool     vt_parse_int(const char *s, long *out);
bool     vt_parse_uint(const char *s, unsigned long *out);
bool     vt_parse_double(const char *s, double *out);
bool     vt_parse_bool(const char *s, bool *out);
const char *vt_bool_str(bool b);
char    *vt_int_to_str(long v);

/* --------------------------------------------------- paths */
const char *vt_home_dir(void);
const char *vt_config_dir(void);
const char *vt_cache_dir(void);
const char *vt_data_dir_user(void);
const char *vt_runtime_dir(void);
char    *vt_path_join(const char *a, const char *b);
char    *vt_path_join_many(const char *first, ...);
bool     vt_path_exists(const char *path);
bool     vt_path_is_dir(const char *path);
bool     vt_path_is_file(const char *path);
char    *vt_path_expand(const char *p);

/* --------------------------------------------------- vectors */
typedef struct vt_vec vt_vec_t;
struct vt_vec {
    void     *data;
    size_t    size;
    size_t    cap;
    size_t    elem_sz;
};

bool  vt_vec_init(vt_vec_t *v, size_t elem_sz, size_t cap);
void  vt_vec_clear(vt_vec_t *v);
void  vt_vec_fini(vt_vec_t *v);
void *vt_vec_push(vt_vec_t *v, const void *elem);
void *vt_vec_push_zero(vt_vec_t *v);
void *vt_vec_at(const vt_vec_t *v, size_t i);
void  vt_vec_pop(vt_vec_t *v);
void  vt_vec_remove(vt_vec_t *v, size_t i);
void *vt_vec_find(const vt_vec_t *v, const void *key,
                  int (*cmp)(const void *a, const void *b));
void  vt_vec_sort(vt_vec_t *v, int (*cmp)(const void *a, const void *b));

/* --------------------------------------------------- linked list */
typedef struct vt_list vt_list_t;
typedef struct vt_node vt_node_t;
struct vt_node {
    vt_node_t *prev, *next;
    void      *data;
};
struct vt_list {
    vt_node_t  head;
    size_t      n;
};

void  vt_list_init(vt_list_t *l);
void  vt_list_fini(vt_list_t *l, void (*free_fn)(void *));
vt_node_t *vt_list_push_back(vt_list_t *l, void *data);
vt_node_t *vt_list_push_front(vt_list_t *l, void *data);
void  vt_list_remove(vt_list_t *l, vt_node_t *n);
void *vt_list_pop_front(vt_list_t *l);
void *vt_list_pop_back(vt_list_t *l);
size_t vt_list_len(const vt_list_t *l);
void  vt_list_foreach(vt_list_t *l, void (*fn)(void *data, void *ud), void *ud);

/* --------------------------------------------------- hash table */
typedef struct vt_hash vt_hash_t;
typedef struct vt_hash_iter {
    vt_hash_t *h;
    size_t      idx;
    void       *cur;
} vt_hash_iter_t;

vt_hash_t *vt_hash_new(size_t init_buckets);
void       vt_hash_free(vt_hash_t *h);
void       vt_hash_free_full(vt_hash_t *h, void (*v_free)(void *));
void      *vt_hash_get(vt_hash_t *h, const char *key);
bool       vt_hash_put(vt_hash_t *h, const char *key, void *val);
bool       vt_hash_put_owned(vt_hash_t *h, const char *key, void *val);
bool       vt_hash_del(vt_hash_t *h, const char *key);
size_t     vt_hash_size(const vt_hash_t *h);
vt_hash_iter_t vt_hash_iter_begin(vt_hash_t *h);
bool       vt_hash_iter_next(vt_hash_iter_t *it, const char **key, void **val);

/* --------------------------------------------------- file */
char *vt_file_read_all(const char *path, size_t *out_len);
bool  vt_file_write_all(const char *path, const void *data, size_t len);
char *vt_file_read_line(FILE *fp);
bool  vt_file_mkdir_p(const char *path, mode_t mode);
bool  vt_file_exists(const char *path);
bool  vt_file_is_dir(const char *path);
char **vt_file_list_dir(const char *path, size_t *out_n);
char   *vt_file_basename(const char *path);
char   *vt_file_dirname(const char *path);

/* --------------------------------------------------- time */
uint64_t vt_time_now_ms(void);
uint64_t vt_time_now_us(void);
uint64_t vt_time_now_ns(void);
void     vt_time_sleep_ms(uint32_t ms);
void     vt_time_format_iso(uint64_t ms, char *buf, size_t n);

/* --------------------------------------------------- RNG */
uint64_t vt_rng_next(void);
void     vt_rng_seed(uint64_t s);
double   vt_rng_uniform(void);

/* --------------------------------------------------- search */
void    *vt_mem_search(const void *hay, size_t hay_n,
                       const void *needle, size_t needle_sz);

#ifdef __cplusplus
}
#endif
#endif
