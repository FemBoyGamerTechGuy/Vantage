/*
 * vt-integ-audio.c — Optional audio integration (PipeWire > Pulse > ALSA)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Audio is NOT a hard dependency. Vantage probes each backend at runtime
 * and uses the first one available. If none are available, all calls
 * return VT_ERR_NOTSUPP and the panel's volume applet shows a disabled
 * indicator.
 */

#define VT_LOG_DOMAIN "integ-audio"
#include <vantage/vt-integrations.h>
#include <string.h>
#include <stdlib.h>

#if defined(VT_HAVE_PULSE)
#include <pulse/pulseaudio.h>
#endif
#if defined(VT_HAVE_PIPEWIRE)
#include <pipewire/pipewire.h>
#endif
#if defined(VT_HAVE_ALSA)
#include <alsa/asoundlib.h>
#endif

struct vt_audio {
    vt_audio_backend_t backend;
    int volume_pct;
    bool muted;
    char *default_sink;
#if defined(VT_HAVE_ALSA)
    snd_mixer_t *alsa_mixer;
#endif
};

vt_audio_t *vt_audio_new(void) {
    vt_audio_t *a = vt_malloc0(sizeof(*a));
    a->backend = VT_AUDIO_BACKEND_NONE;
    return a;
}
void vt_audio_free(vt_audio_t *a) {
    if (!a) return;
    vt_free(a->default_sink);
#if defined(VT_HAVE_ALSA)
    if (a->alsa_mixer) snd_mixer_close(a->alsa_mixer);
#endif
    vt_free(a);
}

vt_audio_backend_t vt_audio_detect(void) {
    /* The only backend with a REAL implementation here is ALSA (direct
     * mixer). PipeWire is consumed through pipewire-alsa, and plain
     * PulseAudio through its ALSA plugin — both surface as "default"
     * ALSA mixers, so ALSA-first detection covers them. Reporting
     * pulse/pipewire without an implementation would produce fake
     * volume numbers, which Vantage never does. */
#if defined(VT_HAVE_ALSA)
    return VT_AUDIO_BACKEND_ALSA;
#endif
    return VT_AUDIO_BACKEND_NONE;
}

const char *vt_audio_backend_str(vt_audio_backend_t b) {
    switch (b) {
    case VT_AUDIO_BACKEND_PIPEWIRE: return "pipewire";
    case VT_AUDIO_BACKEND_PULSE:    return "pulseaudio";
    case VT_AUDIO_BACKEND_ALSA:     return "alsa";
    default:                         return "none";
    }
}

#if defined(VT_HAVE_ALSA)
static snd_mixer_elem_t *_alsa_elem(vt_audio_t *a) {
    if (!a->alsa_mixer) return NULL;
    static const char *const names[] = { "Master", "PCM", "Front",
                                         "Headphone", NULL };
    snd_mixer_selem_id_t *sid = NULL;
    snd_mixer_selem_id_alloca(&sid);
    for (int i = 0; names[i]; i++) {
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, names[i]);
        snd_mixer_elem_t *el = snd_mixer_find_selem(a->alsa_mixer, sid);
        if (el && snd_mixer_selem_has_playback_volume(el)) return el;
    }
    return NULL;
}
static bool _alsa_open(vt_audio_t *a) {
    if (a->alsa_mixer) return true;
    if (snd_mixer_open(&a->alsa_mixer, 0) < 0) return false;
    if (snd_mixer_attach(a->alsa_mixer, "default") < 0 ||
        snd_mixer_selem_register(a->alsa_mixer, NULL, NULL) < 0 ||
        snd_mixer_load(a->alsa_mixer) < 0) {
        snd_mixer_close(a->alsa_mixer);
        a->alsa_mixer = NULL;
        return false;
    }
    return true;
}
#endif

int vt_audio_init(vt_audio_t *a) {
    if (!a) return VT_ERR_INVAL;
    a->backend = vt_audio_detect();
    if (a->backend == VT_AUDIO_BACKEND_NONE) {
        vt_logi("audio: no mixer backend available");
        return VT_ERR_NOTSUPP;
    }
#if defined(VT_HAVE_ALSA)
    if (!_alsa_open(a) || !_alsa_elem(a)) {
        vt_logi("audio: no usable ALSA mixer element "
                "(Master/PCM/Front) — volume display disabled");
        if (a->alsa_mixer) { snd_mixer_close(a->alsa_mixer);
                             a->alsa_mixer = NULL; }
        a->backend = VT_AUDIO_BACKEND_NONE;
        return VT_ERR_NOTSUPP;
    }
    vt_logi("audio: backend = alsa (real mixer values)");
    a->volume_pct = 50;
    a->muted = false;
    return VT_OK;
#else
    vt_logi("audio: built without ALSA — volume display disabled");
    a->backend = VT_AUDIO_BACKEND_NONE;
    return VT_ERR_NOTSUPP;
#endif
}

int vt_audio_set_volume(vt_audio_t *a, int pct) {
    if (!a) return VT_ERR_INVAL;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    a->volume_pct = pct;
#if defined(VT_HAVE_ALSA)
    if (a->backend == VT_AUDIO_BACKEND_ALSA) {
        if (!_alsa_open(a)) return VT_ERR_NOTSUPP;
        snd_mixer_elem_t *el = _alsa_elem(a);
        if (el) {
            long mn = 0, mx = 0;
            snd_mixer_selem_get_playback_volume_range(el, &mn, &mx);
            long v = mn + (mx - mn) * pct / 100;
            snd_mixer_selem_set_playback_volume_all(el, v);
        }
    }
#endif
    return a->backend == VT_AUDIO_BACKEND_NONE ? VT_ERR_NOTSUPP : VT_OK;
}

int vt_audio_get_volume(vt_audio_t *a, int *pct) {
    if (!a) return VT_ERR_INVAL;
#if defined(VT_HAVE_ALSA)
    if (a->backend == VT_AUDIO_BACKEND_ALSA) {
        if (!_alsa_open(a)) return VT_ERR_NOTSUPP;
        snd_mixer_elem_t *el = _alsa_elem(a);
        if (!el) return VT_ERR_NOTSUPP;
        long mn = 0, mx = 0, lv = 0;
        int sw = 1;
        snd_mixer_selem_get_playback_volume_range(el, &mn, &mx);
        snd_mixer_selem_get_playback_volume(el, 0, &lv);
        if (snd_mixer_selem_has_playback_switch(el))
            snd_mixer_selem_get_playback_switch(el, 0, &sw);
        a->volume_pct = mx > mn ? (int)((lv - mn) * 100 / (mx - mn)) : 0;
        a->muted = !sw;
    }
#endif
    if (pct) *pct = a->volume_pct;
    return a->backend == VT_AUDIO_BACKEND_NONE ? VT_ERR_NOTSUPP : VT_OK;
}

int vt_audio_set_mute(vt_audio_t *a, bool mute) {
    if (!a) return VT_ERR_INVAL;
    a->muted = mute;
#if defined(VT_HAVE_ALSA)
    if (a->backend == VT_AUDIO_BACKEND_ALSA) {
        if (!_alsa_open(a)) return VT_ERR_NOTSUPP;
        snd_mixer_elem_t *el = _alsa_elem(a);
        if (el && snd_mixer_selem_has_playback_switch(el))
            snd_mixer_selem_set_playback_switch_all(el, mute ? 0 : 1);
    }
#endif
    return a->backend == VT_AUDIO_BACKEND_NONE ? VT_ERR_NOTSUPP : VT_OK;
}
bool vt_audio_get_mute(vt_audio_t *a) { return a ? a->muted : false; }

const char *vt_audio_default_sink(vt_audio_t *a) {
    if (!a) return NULL;
    vt_free(a->default_sink);
    a->default_sink = vt_strdup(
        a->backend == VT_AUDIO_BACKEND_PIPEWIRE ? "pipewire-default-sink" :
        a->backend == VT_AUDIO_BACKEND_PULSE ?    "pulse-default-sink" :
        a->backend == VT_AUDIO_BACKEND_ALSA ?     "alsa:Master" :
        "none");
    return a->default_sink;
}
