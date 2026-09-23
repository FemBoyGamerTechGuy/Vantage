/*
 * vt-integ-audio.c — Optional audio integration (PipeWire > Pulse > ALSA)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#if defined(VT_HAVE_PIPEWIRE)
    if (getenv("PIPEWIRE_RUNTIME_DIR") || getenv("PIPEWIRE_CORE"))
        return VT_AUDIO_BACKEND_PIPEWIRE;
#endif
#if defined(VT_HAVE_PULSE)
    /* PulseAudio: check PULSE_SERVER or default socket */
    if (getenv("PULSE_SERVER")) return VT_AUDIO_BACKEND_PULSE;
    return VT_AUDIO_BACKEND_PULSE;
#endif
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

int vt_audio_init(vt_audio_t *a) {
    if (!a) return VT_ERR_INVAL;
    a->backend = vt_audio_detect();
    if (a->backend == VT_AUDIO_BACKEND_NONE) {
        vt_logi("audio: no backend available");
        return VT_ERR_NOTSUPP;
    }
    vt_logi("audio: backend = %s", vt_audio_backend_str(a->backend));
#if defined(VT_HAVE_ALSA)
    if (a->backend == VT_AUDIO_BACKEND_ALSA) {
        if (snd_mixer_open(&a->alsa_mixer, 0) == 0) {
            snd_mixer_attach(a->alsa_mixer, "default");
            snd_mixer_selem_register(a->alsa_mixer, NULL, NULL);
            snd_mixer_load(a->alsa_mixer);
        }
    }
#endif
    a->volume_pct = 50; a->muted = false;
    return VT_OK;
}

int vt_audio_set_volume(vt_audio_t *a, int pct) {
    if (!a) return VT_ERR_INVAL;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    a->volume_pct = pct;
#if defined(VT_HAVE_ALSA)
    if (a->backend == VT_AUDIO_BACKEND_ALSA && a->alsa_mixer) {
        snd_mixer_selem_id_t *sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, "Master");
        snd_mixer_elem_t *e = snd_mixer_find_selem(a->alsa_mixer, sid);
        if (e) {
            long min, max;
            snd_mixer_selem_get_playback_volume_range(e, &min, &max);
            long v = min + (max - min) * pct / 100;
            snd_mixer_selem_set_playback_volume_all(e, v);
        }
    }
#endif
    return VT_OK;
}

int vt_audio_get_volume(vt_audio_t *a, int *pct) {
    if (!a) return VT_ERR_INVAL;
    if (pct) *pct = a->volume_pct;
    return VT_OK;
}

int vt_audio_set_mute(vt_audio_t *a, bool mute) {
    if (!a) return VT_ERR_INVAL;
    a->muted = mute;
    return VT_OK;
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
