# Vantage Wallpaper Engine

Vantage supports the following wallpaper kinds:

| Kind      | Description                                              |
|-----------|----------------------------------------------------------|
| `color`   | Solid background color                                   |
| `gradient`| Linear gradient (H/V/diagonal)                          |
| `image`   | Static image (PNG/JPEG via gdk-pixbuf)                   |
| `video`   | **Live video wallpaper** via ffmpeg                      |
| `shader`  | Fragment-shader-generated (requires GPU)                |
| `generated`| Programmatically generated (gradient-like, future)     |

## Setting the wallpaper

```sh
# static image
vantage-config set desktop wallpaper "/path/to/image.png"

# video wallpaper (requires ffmpeg)
vantage-config set desktop video-wallpaper "/path/to/video.mp4"
vantage-config set desktop video-volume 0

# Per-monitor (future feature):
vantage-config set displays.[HDMI-1].wallpaper "/path/to/image.png"
```

## Live video wallpaper

The video wallpaper pipeline:

1. Open the file via libavformat.
2. Locate the first video stream.
3. Allocate an AVCodecContext and decode frames.
4. Convert to RGBA via libswscale.
5. Upload to a GPU texture via `vt_renderer_texture_upload()`.
6. Render at the compositor's frame rate (or pause when not visible).

The pipeline is **event-driven**: it advances a frame only when the
compositor requests a repaint AND the wallpaper surface is visible.
When the wallpaper is occluded by a fullscreen window, the pipeline
pauses and the decoder goes idle.

## Audio

If the video has audio, set `video-volume` to a value between 0 and 1
to enable it. Audio is routed through the chosen audio backend
(PipeWire/PulseAudio/ALSA). Default is 0 (muted) for ambient use.

## Performance notes

* Video wallpaper requires ffmpeg to be linked at build time.
* Hardware-accelerated decode (VAAPI/VDPAU/NVDEC) is used when
  ffmpeg supports it for the codec; the decoded frames are then
  uploaded to a GL texture.
* To verify ffmpeg support: `vantage-diagnostics` shows
  `Video decode: ENABLED` when linked.

## Known limitations

* Audio for video wallpaper is currently routed but not yet
  synchronized with video frames (a/v sync is best-effort).
* The pipeline currently decodes one frame per compositor step.
  For high-FPS videos this is a bottleneck — scheduled to be
  fixed in a follow-up patch.

## Disabling video wallpaper

```sh
vantage-config set desktop video-wallpaper ""
vantage-config set desktop wallpaper "/path/to/static.png"
```

Or use:

```sh
vantage-config set desktop video-volume 0
```

to mute but keep the video.
