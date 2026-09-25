/*
 * vt-image.c — Image loading for wallpapers (libpng + libjpeg direct)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Loads PNG and JPEG files straight into RGBA buffers without pulling in
 * gdk-pixbuf/GLib. This keeps the wallpaper engine dependency-light; if
 * neither library is compiled in, callers fall back to other loaders.
 */

#define VT_LOG_DOMAIN "image"
#include <vantage/vt-core.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(VT_HAVE_PNG)
#include <png.h>
#endif
#if defined(VT_HAVE_JPEG)
#include <jpeglib.h>
#include <setjmp.h>
#endif

bool vt_image_probe_png(const char *path);
bool vt_image_probe_jpeg(const char *path);
/* Load `path` into a heap RGBA buffer (caller frees). Returns NULL on
 * failure. The out_w and out_h pointers receive the dimensions. */
uint8_t *vt_image_load_png(const char *path, int *out_w, int *out_h);
uint8_t *vt_image_load_jpeg(const char *path, int *out_w, int *out_h);

bool vt_image_probe_png(const char *path) {
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char sig[8];
    size_t n = fread(sig, 1, 8, f);
    fclose(f);
    return n == 8 && png_sig_cmp(sig, 0, 8) == 0;
}

bool vt_image_probe_jpeg(const char *path) {
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char sig[3];
    size_t n = fread(sig, 1, 3, f);
    fclose(f);
    return n == 3 && sig[0] == 0xff && sig[1] == 0xd8 && sig[2] == 0xff;
}

#if defined(VT_HAVE_PNG)
uint8_t *vt_image_load_png(const char *path, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL,
                                             NULL, NULL);
    if (!png) { fclose(f); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(f); return NULL; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return NULL;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    int w = (int)png_get_image_width(png, info);
    int h = (int)png_get_image_height(png, info);
    png_byte color = png_get_color_type(png, info);
    png_byte depth = png_get_bit_depth(png, info);
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xff, PNG_FILLER_AFTER);   /* ensure alpha channel */
    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    uint8_t *data = vt_malloc0((size_t)h * rowbytes);
    png_bytep *rows = vt_malloc(sizeof(png_bytep) * (size_t)h);
    for (int y = 0; y < h; y++)
        rows[y] = data + (size_t)y * rowbytes;
    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    vt_free(rows);
    fclose(f);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    /* normalize stride to w*4 (png gives that after filler, but be safe) */
    if (rowbytes == (size_t)w * 4) return data;
    uint8_t *packed = vt_malloc0((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(packed + (size_t)y * w * 4, data + (size_t)y * rowbytes,
               (size_t)w * 4);
    vt_free(data);
    return packed;
}
#else
uint8_t *vt_image_load_png(const char *path, int *out_w, int *out_h) {
    (void)path; (void)out_w; (void)out_h;
    return NULL;
}
#endif

#if defined(VT_HAVE_JPEG)
struct _jpeg_err_wrap {
    struct jpeg_error_mgr mgr;
    jmp_buf jb;
};

static void _jpeg_error_exit(j_common_ptr cinfo) {
    struct _jpeg_err_wrap *e = (struct _jpeg_err_wrap *)cinfo->err;
    longjmp(e->jb, 1);
}

static void _jpeg_emit_message(j_common_ptr cinfo, int msg_level) {
    (void)cinfo; (void)msg_level; /* silence warnings */
}

uint8_t *vt_image_load_jpeg(const char *path, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct jpeg_decompress_struct cinfo;
    struct _jpeg_err_wrap jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = _jpeg_error_exit;
    jerr.mgr.emit_message = _jpeg_emit_message;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return NULL;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    uint8_t *data = vt_malloc0((size_t)w * h * 4);
    uint8_t *row = vt_malloc((size_t)w * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        int y = (int)cinfo.output_scanline;
        JSAMPROW rp = row;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        for (int x = 0; x < w; x++) {
            uint8_t *dst = data + ((size_t)y * w + x) * 4;
            dst[0] = row[x * 3 + 0];
            dst[1] = row[x * 3 + 1];
            dst[2] = row[x * 3 + 2];
            dst[3] = 0xff;
        }
    }
    vt_free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return data;
}
#else
uint8_t *vt_image_load_jpeg(const char *path, int *out_w, int *out_h) {
    (void)path; (void)out_w; (void)out_h;
    return NULL;
}
#endif
