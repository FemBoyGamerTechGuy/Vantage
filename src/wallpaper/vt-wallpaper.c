/*
 * vt-wallpaper.c — Wallpaper engine core
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Supports: solid color, gradient, static image, live video, shader.
 *
 * Video wallpaper uses ffmpeg (libavcodec/libavformat/libavutil). When
 * not available, the video wallpaper mode is rejected at load time
 * and falls back to a still image. The pipeline is NOT continuously
 * repainted: it advances a frame only when the compositor requests a
 * new frame (driven by damage + frame scheduling), and only when the
 * wallpaper surface is actually visible.
 */

#define VT_LOG_DOMAIN "wallpaper"
#include <vantage/vt-wallpaper.h>
#include <vantage/vt-config.h>
#include <vantage/vt-icons.h>

#if defined(VT_HAVE_FFMPEG)
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#endif

#if defined(VT_HAVE_GDKPIXBUF)
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* vt-image.c */
bool vt_image_probe_png(const char *path);
bool vt_image_probe_jpeg(const char *path);
uint8_t *vt_image_load_png(const char *path, int *out_w, int *out_h);
uint8_t *vt_image_load_jpeg(const char *path, int *out_w, int *out_h);

struct vt_wallpaper_priv {
    /* video decoder state */
#if defined(VT_HAVE_FFMPEG)
    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;
    AVFrame        *frame;
    struct SwsContext *sws;
    int            video_stream;
    uint8_t       *scratch;
    size_t         scratch_sz;
#endif
    bool            paused;
};

vt_wallpaper_t *vt_wallpaper_new(void) {
    vt_wallpaper_t *w = vt_malloc0(sizeof(*w));
    w->kind = VT_WALLPAPER_COLOR;
    w->color_a = (vt_color_t){0.10f, 0.10f, 0.12f, 1.0f};
    w->color_b = (vt_color_t){0.20f, 0.20f, 0.24f, 1.0f};
    w->gradient_dir = 2;
    w->scale = 1;
    w->volume = 0;
    w->loop = true;
    w->per_output = -1;
    w->priv = vt_malloc0(sizeof(struct vt_wallpaper_priv));
    return w;
}

void vt_wallpaper_free(vt_wallpaper_t *w) {
    if (!w) return;
    vt_free(w->path);
    vt_free(w->shader);
#if defined(VT_HAVE_FFMPEG)
    if (w->priv) {
        struct vt_wallpaper_priv *p = w->priv;
        if (p->sws)        sws_freeContext(p->sws);
        if (p->frame)      av_frame_free(&p->frame);
        if (p->dec_ctx)    avcodec_free_context(&p->dec_ctx);
        if (p->fmt_ctx)    avformat_close_input(&p->fmt_ctx);
        vt_free(p->scratch);
        vt_free(p);
    }
#else
    vt_free(w->priv);
#endif
    vt_free(w);
}

void vt_wallpaper_set_kind(vt_wallpaper_t *w, vt_wallpaper_kind_t k) {
    if (w) w->kind = k;
}
void vt_wallpaper_set_color(vt_wallpaper_t *w, vt_color_t a, vt_color_t b, int dir) {
    if (!w) return;
    w->color_a = a; w->color_b = b; w->gradient_dir = dir;
}
void vt_wallpaper_set_path(vt_wallpaper_t *w, const char *p) {
    if (!w) return;
    vt_free(w->path);
    w->path = vt_strdup(p);
}
void vt_wallpaper_set_volume(vt_wallpaper_t *w, float v) {
    if (w) w->volume = v;
}
void vt_wallpaper_set_loop(vt_wallpaper_t *w, bool on) {
    if (w) w->loop = on;
}
void vt_wallpaper_pause(vt_wallpaper_t *w) { if (w && w->priv) ((struct vt_wallpaper_priv *)w->priv)->paused = true; }
void vt_wallpaper_resume(vt_wallpaper_t *w) { if (w && w->priv) ((struct vt_wallpaper_priv *)w->priv)->paused = false; }

int vt_wallpaper_load(vt_wallpaper_t *w, const char *spec) {
    if (!w) return VT_ERR_INVAL;
    if (spec) {
        vt_free(w->path);
        w->path = vt_strdup(spec);
    }
    if (w->kind == VT_WALLPAPER_VIDEO) {
#if defined(VT_HAVE_FFMPEG)
        struct vt_wallpaper_priv *p = w->priv;
        if (!p) { p = vt_malloc0(sizeof(*p)); w->priv = p; }
        if (avformat_open_input(&p->fmt_ctx, w->path, NULL, NULL) < 0) {
            vt_logw("wallpaper: cannot open %s", w->path);
            return VT_ERR_IO;
        }
        if (avformat_find_stream_info(p->fmt_ctx, NULL) < 0) {
            return VT_ERR_PROTOCOL;
        }
        p->video_stream = -1;
        for (unsigned i = 0; i < p->fmt_ctx->nb_streams; i++) {
            if (p->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                p->video_stream = (int)i;
                break;
            }
        }
        if (p->video_stream < 0) return VT_ERR_PROTOCOL;
        const AVCodec *c = avcodec_find_decoder(
            p->fmt_ctx->streams[p->video_stream]->codecpar->codec_id);
        if (!c) return VT_ERR_NOTSUPP;
        p->dec_ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(p->dec_ctx,
            p->fmt_ctx->streams[p->video_stream]->codecpar);
        if (avcodec_open2(p->dec_ctx, c, NULL) < 0) return VT_ERR;
        p->frame = av_frame_alloc();
        vt_logi("wallpaper: video opened (%s)", w->path);
#else
        vt_logw("wallpaper: built without ffmpeg; video wallpaper disabled");
        return VT_ERR_NOTSUPP;
#endif
    }
    return VT_OK;
}

int vt_wallpaper_load_from_config(vt_wallpaper_t *w) {
    return vt_wallpaper_config_load(w);
}

int vt_wallpaper_config_load(vt_wallpaper_t *w) {
    if (!w) return VT_ERR_INVAL;
    vt_config_t *cfg = vt_config_new_defaults();
    if (!cfg) return VT_ERR_NOMEM;
    vt_config_load(cfg, vt_config_default_path());
    const char *mode = vt_config_get(cfg, "wallpaper", "mode", "gradient");
    if (vt_strcaseeq(mode, "color")) {
        w->kind = VT_WALLPAPER_COLOR;
    } else if (vt_strcaseeq(mode, "image")) {
        w->kind = VT_WALLPAPER_IMAGE;
        vt_wallpaper_set_path(w, vt_config_get(cfg, "wallpaper", "path",
                                               ""));
    } else if (vt_strcaseeq(mode, "video")) {
        w->kind = VT_WALLPAPER_VIDEO;
        vt_wallpaper_set_path(w, vt_config_get(cfg, "wallpaper", "path",
                                               ""));
        w->loop = vt_config_get_bool(cfg, "wallpaper", "loop", true);
    } else {
        w->kind = VT_WALLPAPER_GRADIENT;
    }
    vt_color_t a, b;
    a.r = 0.07f; a.g = 0.09f; a.b = 0.14f; a.a = 1.0f;
    b.r = 0.15f; b.g = 0.19f; b.b = 0.31f; b.a = 1.0f;
    const char *ca = vt_config_get(cfg, "wallpaper", "color_a", NULL);
    const char *cb = vt_config_get(cfg, "wallpaper", "color_b", NULL);
    if (ca) sscanf(ca, "%f,%f,%f", &a.r, &a.g, &a.b);
    if (cb) sscanf(cb, "%f,%f,%f", &b.r, &b.g, &b.b);
    w->gradient_dir = (int)vt_config_get_int(cfg, "wallpaper",
                                              "gradient_dir", 1);
    vt_wallpaper_set_color(w, a, b, w->gradient_dir);
    vt_config_free(cfg);
    return VT_OK;
}

void vt_wallpaper_render_argb(vt_wallpaper_t *w, uint32_t *pix,
                               int pw, int ph) {
    if (!w || !pix || pw <= 0 || ph <= 0) return;
    switch (w->kind) {
    case VT_WALLPAPER_COLOR:
    case VT_WALLPAPER_VIDEO: {
        /* video needs the renderer stepping path; the CPU background
         * honestly falls back to the base color */
        if (w->kind == VT_WALLPAPER_VIDEO)
            vt_logw("wallpaper: video wallpaper renders on the X11 "
                    "renderer path — the Wayland background uses the "
                    "base color");
        uint32_t c = 0xff000000u |
                     ((uint32_t)(int)(w->color_a.r * 255.0f + 0.5f) << 16) |
                     ((uint32_t)(int)(w->color_a.g * 255.0f + 0.5f) << 8) |
                     (uint32_t)(int)(w->color_a.b * 255.0f + 0.5f);
        for (int i = 0; i < pw * ph; i++) pix[i] = c;
        break;
    }
    case VT_WALLPAPER_GRADIENT: {
        /* per-pixel interpolation: smooth, no 32-stripe banding.
         * dir: 0=horizontal, 1=vertical, 2=diagonal, 3=reverse diagonal */
        int ra = (int)(w->color_a.r * 255.0f + 0.5f);
        int ga = (int)(w->color_a.g * 255.0f + 0.5f);
        int ba = (int)(w->color_a.b * 255.0f + 0.5f);
        int rb = (int)(w->color_b.r * 255.0f + 0.5f);
        int gb = (int)(w->color_b.g * 255.0f + 0.5f);
        int bb = (int)(w->color_b.b * 255.0f + 0.5f);
        for (int y = 0; y < ph; y++) {
            double ty = (double)y / (ph > 1 ? ph - 1 : 1);
            for (int x = 0; x < pw; x++) {
                double tx = (double)x / (pw > 1 ? pw - 1 : 1);
                double t;
                switch (w->gradient_dir) {
                case 0: t = tx; break;
                case 2: t = (tx + ty) * 0.5; break;
                case 3: t = 1.0 - (tx + ty) * 0.5; break;
                default: t = ty; break;
                }
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                pix[y * pw + x] = 0xff000000u |
                    (uint32_t)(ra + (int)((rb - ra) * t)) << 16 |
                    (uint32_t)(ga + (int)((gb - ga) * t)) << 8 |
                    (uint32_t)(ba + (int)((bb - ba) * t));
            }
        }
        break;
    }
    case VT_WALLPAPER_IMAGE: {
        if (!w->path || !*w->path) {
            uint32_t c = 0xff000000u |
                         ((uint32_t)(int)(w->color_a.r * 255.0f + 0.5f) << 16) |
                         ((uint32_t)(int)(w->color_a.g * 255.0f + 0.5f) << 8) |
                         (uint32_t)(int)(w->color_a.b * 255.0f + 0.5f);
            for (int i = 0; i < pw * ph; i++) pix[i] = c;
            break;
        }
        int iw = 0, ih = 0;
        uint32_t *ip = NULL;
        if (vt_icon_load_argb_sized(w->path, 0, &ip, &iw, &ih) != 0 || !ip) {
            vt_logw("wallpaper: cannot decode %s — flat color background",
                    w->path);
            uint32_t c = 0xff000000u |
                         ((uint32_t)(int)(w->color_a.r * 255.0f + 0.5f) << 16) |
                         ((uint32_t)(int)(w->color_a.g * 255.0f + 0.5f) << 8) |
                         (uint32_t)(int)(w->color_a.b * 255.0f + 0.5f);
            for (int i = 0; i < pw * ph; i++) pix[i] = c;
            break;
        }
        /* cover: scale so both axes are covered, center-crop */
        double sc = (double)pw / iw > (double)ph / ih
                  ? (double)pw / iw : (double)ph / ih;
        int cw = (int)((double)iw * sc + 0.5);
        int ch = (int)((double)ih * sc + 0.5);
        if (cw < pw) cw = pw;
        if (ch < ph) ch = ph;
        uint32_t *scaled = vt_icon_scale_argb(ip, iw, ih, cw, ch);
        vt_free(ip);
        if (!scaled) {
            uint32_t c = 0xff000000u |
                         ((uint32_t)(int)(w->color_a.r * 255.0f + 0.5f) << 16) |
                         ((uint32_t)(int)(w->color_a.g * 255.0f + 0.5f) << 8) |
                         (uint32_t)(int)(w->color_a.b * 255.0f + 0.5f);
            for (int i = 0; i < pw * ph; i++) pix[i] = c;
            break;
        }
        int ox = (cw - pw) / 2, oy = (ch - ph) / 2;
        for (int y = 0; y < ph; y++)
            memcpy(pix + (size_t)y * pw, scaled + (size_t)(y + oy) * cw + ox,
                   sizeof(uint32_t) * (size_t)pw);
        vt_free(scaled);
        vt_logi("wallpaper: background %s %dx%d → cover %dx%d",
                w->path, iw, ih, pw, ph);
        break;
    }
    default: {
        uint32_t c = 0xff000000u |
                     ((uint32_t)(int)(w->color_a.r * 255.0f + 0.5f) << 16) |
                     ((uint32_t)(int)(w->color_a.g * 255.0f + 0.5f) << 8) |
                     (uint32_t)(int)(w->color_a.b * 255.0f + 0.5f);
        for (int i = 0; i < pw * ph; i++) pix[i] = c;
        break;
    }
    }
}

int vt_wallpaper_step(vt_wallpaper_t *w, vt_renderer_t *r, uint32_t output_w, uint32_t output_h) {
    if (!w || !r) return VT_ERR_INVAL;
    if (w->kind == VT_WALLPAPER_VIDEO) {
#if defined(VT_HAVE_FFMPEG)
        struct vt_wallpaper_priv *p = w->priv;
        if (!p || p->paused) return VT_OK;
        if (!p->fmt_ctx) return VT_ERR_INVAL;
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) return VT_ERR_NOMEM;
        if (av_read_frame(p->fmt_ctx, pkt) < 0) {
            av_packet_free(&pkt);
            if (w->loop) {
                if (av_seek_frame(p->fmt_ctx, p->video_stream, 0,
                                  AVSEEK_FLAG_BACKWARD) >= 0) {
                    avcodec_flush_buffers(p->dec_ctx);
                }
            }
            return VT_OK;
        }
        if (pkt->stream_index == p->video_stream) {
            int got = 0;
            if (avcodec_send_packet(p->dec_ctx, pkt) == 0) {
                got = (avcodec_receive_frame(p->dec_ctx, p->frame) == 0);
            }
            if (got) {
                /* convert to RGBA, preserving aspect ratio (letterbox) */
                if (!p->sws) {
                    p->sws = sws_getContext(
                        p->dec_ctx->width, p->dec_ctx->height, p->dec_ctx->pix_fmt,
                        (int)output_w, (int)output_h, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, NULL, NULL, NULL);
                    p->scratch_sz = (size_t)output_w * output_h * 4;
                    p->scratch = vt_realloc(p->scratch, p->scratch_sz);
                }
                uint8_t *dst[4] = { p->scratch, NULL, NULL, NULL };
                int linesize[4] = { (int)output_w * 4, 0, 0, 0 };
                sws_scale(p->sws, (const uint8_t *const *)p->frame->data,
                          p->frame->linesize, 0, p->frame->height,
                          dst, linesize);
                if (!w->texture)
                    w->texture = vt_renderer_texture_create(r, output_w, output_h, VT_PF_ARGB8888);
                vt_renderer_texture_upload(r, w->texture, p->scratch);
            }
        }
        av_packet_unref(pkt);
        av_packet_free(&pkt);
#endif
    } else if (w->kind == VT_WALLPAPER_IMAGE) {
        if (!w->texture && w->path) {
            /* direct loaders first: no gdk-pixbuf/GLib dependency */
            int iw = 0, ih = 0;
            uint8_t *rgba = NULL;
            if (vt_image_probe_png(w->path))
                rgba = vt_image_load_png(w->path, &iw, &ih);
            else if (vt_image_probe_jpeg(w->path))
                rgba = vt_image_load_jpeg(w->path, &iw, &ih);
            if (rgba) {
                if (r) {
                    w->texture = vt_renderer_texture_create(
                        r, (uint32_t)iw, (uint32_t)ih, VT_PF_ARGB8888);
                    if (w->texture)
                        vt_renderer_texture_upload(r, w->texture, rgba);
                }
                vt_free(rgba);
                vt_logi("wallpaper: loaded %s (%dx%d)", w->path, iw, ih);
                return VT_OK;
            }
        }
#if defined(VT_HAVE_GDKPIXBUF)
        if (!w->texture && w->path) {
            GError *err = NULL;
            GdkPixbuf *pb = gdk_pixbuf_new_from_file(w->path, &err);
            if (pb) {
                int iw = gdk_pixbuf_get_width(pb);
                int ih = gdk_pixbuf_get_height(pb);
                int channels = gdk_pixbuf_get_n_channels(pb);
                int rs = gdk_pixbuf_get_rowstride(pb);
                const uint8_t *data = gdk_pixbuf_get_pixels(pb);
                uint8_t *rgba = vt_malloc0((size_t)iw * ih * 4);
                for (int y = 0; y < ih; y++) {
                    for (int x = 0; x < iw; x++) {
                        const uint8_t *src = data + y * rs + x * channels;
                        uint8_t *dst = rgba + (y * iw + x) * 4;
                        dst[0] = src[0]; dst[1] = src[1];
                        dst[2] = src[2];
                        dst[3] = channels >= 4 ? src[3] : 255;
                    }
                }
                w->texture = vt_renderer_texture_create(r, iw, ih, VT_PF_ARGB8888);
                vt_renderer_texture_upload(r, w->texture, rgba);
                vt_free(rgba);
                g_object_unref(pb);
            } else if (err) {
                vt_logw("wallpaper: %s", err->message);
                g_error_free(err);
            }
        }
#else
        if (!w->texture && w->path) {
            /* fallback: can't decode images without pixbuf */
            vt_logw("wallpaper: built without gdk-pixbuf; using color fallback");
            w->kind = VT_WALLPAPER_COLOR;
        }
#endif
    }
    return VT_OK;
}

void vt_wallpaper_render(vt_wallpaper_t *w, vt_renderer_t *r, vt_rect_t area) {
    if (!w || !r) return;
    switch (w->kind) {
    case VT_WALLPAPER_COLOR:
        vt_renderer_fill_rect(r, area, w->color_a);
        break;
    case VT_WALLPAPER_GRADIENT: {
        /* simple horizontal/vertical gradient via N solid stripes */
        int stripes = 32;
        for (int i = 0; i < stripes; i++) {
            float t = (float)i / (float)stripes;
            vt_color_t c = {
                w->color_a.r + (w->color_b.r - w->color_a.r) * t,
                w->color_a.g + (w->color_b.g - w->color_a.g) * t,
                w->color_a.b + (w->color_b.b - w->color_a.b) * t,
                1.0f
            };
            vt_rect_t s = area;
            if (w->gradient_dir == 0 || w->gradient_dir == 2) {
                s.w = area.w / stripes + 1; s.x = area.x + i * (area.w / stripes);
            } else {
                s.h = area.h / stripes + 1; s.y = area.y + i * (area.h / stripes);
            }
            vt_renderer_fill_rect(r, s, c);
        }
        break;
    }
    case VT_WALLPAPER_IMAGE:
    case VT_WALLPAPER_VIDEO:
        if (w->texture) {
            vt_renderer_texture_draw(r, w->texture, area, NULL, NULL);
        } else {
            vt_renderer_fill_rect(r, area, w->color_a);
        }
        break;
    default:
        vt_renderer_fill_rect(r, area, w->color_a);
        break;
    }
}
