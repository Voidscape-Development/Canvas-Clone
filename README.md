# Canvas Clone

An OBS Studio plugin that adds a **Canvas Clone** source: it mirrors the finished picture of another
canvas — the main program canvas, a vertical canvas, or any canvas registered by another plugin — so
you can place that feed inside a different canvas as if it were a live video source.

The typical use: drop the **Main Canvas (Program)** feed into a vertical canvas. Scene switches,
transitions, stingers, filters and everything else that happens on the main canvas show up in the
vertical feed exactly as viewers of the main stream see it, so social clips keep the full production
instead of a static crop.

## Requirements

* OBS Studio **31.1.0 or newer** — the plugin is built on the multi-canvas API (`obs_canvas_t`) that
  shipped in 31.1. The module refuses to load on older versions.
* Windows, macOS or Linux.

## Usage

Add a source → **Canvas Clone**, then:

| Property | What it does |
|----------|--------------|
| **Clone** | Whether to mirror a whole canvas or a single scene/source. |
| **Canvas** | The canvas to mirror. Lists the main canvas plus every canvas registered by another plugin. |
| **Scene / Source** | In scene/source mode, the scene or source to mirror (scenes belonging to another canvas are prefixed with that canvas's name). |
| **Render Mode** | How the picture is taken — see below. |
| **Audio** | Off, mirror the cloned canvas/source's audio, or mirror a specific source. |

The source reports the cloned canvas's base resolution (e.g. 1920x1080), so scale, crop and position
it with the normal scene transform — a bounding box set to "Scale to inner bounds" is the usual way
to fit a 16:9 canvas into a 9:16 one.

### Render modes

**Canvas output (up to one frame behind)** — the default. Copies the canvas's finished output
texture, so the clone is pixel-identical to a projector or recording of that canvas, costs one
texture copy per frame regardless of how complex the scene is, and is safe to place *inside* the
canvas it is cloning (that produces the familiar infinite-mirror effect, one frame deep per level).

**Live re-render (no added delay)** — draws the cloned canvas's program source again in place, in
perfect sync with the canvas it is drawn into. The whole scene tree and its GPU filters render a
second time each frame, so it costs more on heavy scenes. A clone placed inside the canvas it clones
renders nothing on re-entry, since there is no buffered frame to fall back on.

### Audio

Audio is off by default: the cloned canvas's audio is usually already in your mix, and cloning it
again would double it up. When enabled, the source appears in the audio mixer and mirrors the
target's existing audio mix — including transition fades and each source's own volume — which you
can then mute, adjust or route to specific tracks like any other source.

Because libobs has no per-canvas audio mix (canvas audio folds into the main mix), the mirror reads
the mix of the canvas's *program source*. That mix only exists while the target is live somewhere in
OBS; a canvas or source that nothing else is rendering produces no audio to mirror.

### Notes and limits

* A canvas owned by another plugin may be recreated with a new UUID between launches, so the
  canvas's name is saved alongside its UUID and used as a fallback. A canvas that hasn't loaded yet
  stays selected in the properties list, marked `(not loaded)`, and reconnects on its own once it
  appears.
* HDR canvases are tonemapped when cloned into an SDR canvas, the same way OBS tonemaps a projector.
* Canvas output is premultiplied and is converted back to straight alpha before being composited, so
  scene blend modes and item opacity behave normally.

## Building

The project uses the standard OBS plugin build system (CMake presets + the obs-deps toolchain);
see the [plugin template wiki](https://github.com/obsproject/obs-plugintemplate/wiki) for the
per-platform prerequisites.

```sh
cmake --preset ubuntu-x86_64   # or windows-x64 / macos
cmake --build --preset ubuntu-x86_64
```

GitHub Actions builds Windows, macOS and Ubuntu artifacts on every push and pull request; pushing a
semver tag (e.g. `1.0.0`) produces a draft release with installer packages attached.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
