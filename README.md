# gasio — Low-latency ASIO audio I/O for Godot

A Godot 4.6 GDExtension for **ASIO** audio input, output, and full-duplex on
Windows, giving much lower latency than the standard OS audio path. Built for
real-time uses like instrument input, monitoring, tuners, and rhythm/music games.

> **Platform:** the ASIO API is Windows-only. On Linux/macOS the extension is not
> loaded and the game falls back to Godot's built-in audio (ALSA/PulseAudio /
> CoreAudio). The included routing and pitch/chord detection scripts work on
> every platform.

---

## Why

Godot's standard audio goes through the OS mixer and carries significant latency.
ASIO (Audio Stream Input/Output) talks much closer to the hardware, which is
essential when the player hears or reacts to their own input in real time —
guitar/bass input, live monitoring, tuners, beat-matching, etc.

---

## What it provides

Three GDExtension classes (Windows only):

| Class | Role |
|-------|------|
| `AsioAudioInput`  | Capture from an ASIO driver. Emits `audio_frame` per channel. |
| `AsioAudioOutput` | Play to an ASIO driver. Fed with `push_frames`. |
| `AsioAudioDevice` | **Full-duplex**: one driver, input + output together, with direct on-thread monitoring. The right choice for one interface doing both. |

> ⚠️ ASIO allows only **one open driver per process**. Don't open the same
> interface with `AsioAudioInput` *and* `AsioAudioOutput` at once — use
> `AsioAudioDevice` for full duplex. The bundled `AudioRouter` handles this
> automatically (see below).

Plus GDScript helpers (in `test_project/`): an `AudioRouter` that coordinates all
this, input/output device selectors, and YIN/spectrum pitch & chord detectors.

---

## Platform support

| Platform | ASIO I/O | Fallback | Routing + detection |
|----------|:--------:|----------|:-------------------:|
| Windows  | ✅        | WASAPI   | ✅ |
| Linux    | ❌ (absent) | ALSA / PulseAudio | ✅ |
| macOS    | ❌ (absent) | CoreAudio | ✅ |

On non-Windows platforms the ASIO classes don't exist. Guard usage with
`ClassDB.class_exists("AsioAudioInput")` (etc.) — the bundled scripts already do.

- **Godot version:** 4.6+ (`compatibility_minimum = "4.6"`)

---

## Installation

1. Copy the `gasio/` folder (containing `gasio.gdextension` and `bin/`) into your
   project. It is self-contained — library paths are relative to the
   `.gdextension`, so the folder can live anywhere.
2. Restart the Godot editor so the extension loads.

### Project settings prerequisites

- **Project Settings → Audio → Driver → Enable Input** must be ON.
- Create a bus named **`Record`** in the Audio panel — the detectors and the
  router attach their capture/analysis effects here.

---

## Quick start

### Input (capture)

```gdscript
func _ready() -> void:
    if not ClassDB.class_exists("AsioAudioInput"):
        return # not on Windows
    var asio := AsioAudioInput.new()
    add_child(asio)
    var drivers := asio.get_driver_list()
    if drivers.is_empty():
        return
    asio.audio_frame.connect(_on_audio_frame)
    if asio.open(drivers[0], -1): # -1 = minimum buffer (lowest latency)
        asio.start()

func _on_audio_frame(data: PackedFloat32Array, channel: int) -> void:
    pass # one block of normalized float samples for `channel`
```

### Full-duplex monitoring (one interface, in + out)

```gdscript
var dev = ClassDB.instantiate("AsioAudioDevice")
add_child(dev)
if dev.open("Focusrite USB ASIO", 0):
    dev.set_direct_monitor(true) # hear input through output, ASIO-thread latency
    dev.set_monitor_gain(1.0)
    dev.audio_frame.connect(_on_audio_frame) # optional, for detection
    dev.start()
```

In-editor class references are generated from `doc_classes/*.xml`
([AsioAudioInput](doc_classes/AsioAudioInput.xml),
[AsioAudioOutput](doc_classes/AsioAudioOutput.xml),
[AsioAudioDevice](doc_classes/AsioAudioDevice.xml)).

### Buffer size & sample rate (read this)

- `open(name, buffer_size)`: `0` = driver preferred, `-1` = minimum (lowest
  latency), positive = clamped to the supported range.
- Match sample rates: set the ASIO driver to `AudioServer.get_mix_rate()` (e.g.
  48000) and any `AudioStreamGenerator.mix_rate` to the driver rate. A mismatch
  causes pitch/speed errors and under/overruns.
- For **output**, prefer `0` (preferred). A tiny buffer gives no latency benefit
  on the push-based output path and risks underruns.

---

## The AudioRouter (recommended)

`test_project/utils/audio_router.gd` is an autoload singleton that routes **all**
audio — native and ASIO — and resolves the one-driver constraint for you. The
selectors report the user's choice to it and it sets up the right objects:

| Input pick | Output pick | Router opens |
|------------|-------------|--------------|
| ASIO | ASIO (same driver) | one full-duplex `AsioAudioDevice` (direct monitor + game audio out) |
| ASIO | native | `AsioAudioInput` → `Record` bus; native output |
| native | ASIO | native input → `Record`; `AsioAudioOutput` from a Master capture |
| native | native | Godot's native devices |

It feeds ASIO input into the `Record` bus (so the detectors work in every mode,
including full-duplex), mutes the OS output when ASIO output is active, and
tears down/reopens drivers safely when the selection changes.

Register it as an autoload (already set in the demo's `project.godot`):

```
[autoload]
AudioRouter="*res://utils/audio_router.gd"
```

---

## Demo project

[`test_project/`](test_project/) is a runnable project wiring everything
together. Its [main.tscn](test_project/main.tscn) has:

- two **OptionButton**s — [utils/input_selector.gd](test_project/utils/input_selector.gd)
  and [utils/output_selector.gd](test_project/utils/output_selector.gd) — listing
  native and ASIO devices; selections go to the `AudioRouter` autoload;
- a **pitch_detector** ([music/pitch_detector.gd](test_project/music/pitch_detector.gd))
  reading the `Record` bus;
- a **pitch_detector_test** ([music/pitch_detector_test.gd](test_project/music/pitch_detector_test.gd))
  printing detected notes.

### Running it

1. Open `test_project/` in Godot 4.6.
2. Let the editor build/load the GDExtension; reload if prompted.
3. Confirm **Audio → Enable Input** is on and a **`Record`** bus exists.
4. Press **Play**. Pick an input and an output from the dropdowns.
   - Same ASIO driver for both → automatic full-duplex monitoring.
5. Play into the input and watch the **Output** panel: notes print as
   `E2 (82.6 Hz, -18.3 dB, ch1, -7¢)`.

For chords, swap the detector to
[music/pitch_detector_chord.gd](test_project/music/pitch_detector_chord.gd) and
the test to [music/pitch_detector_test_chord.gd](test_project/music/pitch_detector_test_chord.gd)
(e.g. an E power chord → `E5`).

---

## What's included

| File | Purpose |
|------|---------|
| `AsioAudioInput` / `AsioAudioOutput` / `AsioAudioDevice` (C++) | The ASIO capture / playback / full-duplex nodes. |
| [utils/audio_router.gd](test_project/utils/audio_router.gd) | Autoload coordinating native + ASIO routing and full-duplex. |
| [utils/input_selector.gd](test_project/utils/input_selector.gd) / [utils/output_selector.gd](test_project/utils/output_selector.gd) | Dropdowns listing native and ASIO devices; report to the router. |
| [music/pitch_detector.gd](test_project/music/pitch_detector.gd) | Monophonic note detection (YIN), emits `NoteEvent`. |
| [music/pitch_detector_chord.gd](test_project/music/pitch_detector_chord.gd) | Polyphonic chord detection (spectrum), emits `ChordEvent`. |
| [music/note_event.gd](test_project/music/note_event.gd) / [music/chord_event.gd](test_project/music/chord_event.gd) | Value objects for detection results. |
| [music/pitch_detector_test.gd](test_project/music/pitch_detector_test.gd) / [music/pitch_detector_test_chord.gd](test_project/music/pitch_detector_test_chord.gd) | Minimal examples that print events. |

Detection range is **E2 (82.41 Hz) – C6 (1046.50 Hz)**. Detectors read the
`Record` bus, independent of the input source.

---

## Latency guide

For **monitoring** (hearing your input), full-duplex `AsioAudioDevice` with
`set_direct_monitor(true)` is the low-latency path — input is copied to the
output entirely on the ASIO callback thread, with no main-thread hop.

When audio instead flows through Godot (input → bus → output, or `push_frames`),
the round-trip is dominated by the shared Godot stages:

```
capture  →  main-thread hop (~16ms @60fps)  →  generator/ring buffer
   →  Godot output latency (~15ms)  →  speakers
```

To reduce that path:

1. **Lower Godot's output latency** — Project Settings → Audio → Output Latency
   (e.g. 15 → 5 ms). Biggest single win.
2. **Match sample rates** (driver ↔ `AudioServer.get_mix_rate()`).
3. **Use a native ASIO interface** (Focusrite/RME/MOTU). ASIO4ALL is a WASAPI
   wrapper with a similar floor to plain WASAPI.
4. For true near-zero monitoring, use `AsioAudioDevice` direct monitor (above)
   rather than routing through Godot's mixer.

---

## Known limitations

- **Windows-only** for ASIO (by design).
- **One driver per process:** don't open the same interface with the separate
  input and output classes at once — use `AsioAudioDevice`. The `AudioRouter`
  enforces this automatically.
- **ASIO4ALL exclusive-mode contention:** a device held by ASIO can't also be
  used by WASAPI (and vice versa). The router parks/releases devices on switch.
- **Chord detection** is approximate: harmonics can add phantom notes (raise
  `presence_ratio`), and low-note resolution is limited even at FFT 4096.
- **`double` precision is not supported** — godot-cpp ships a single-precision
  `extension_api.json`.

---

## Exporting your game

The `gasio/` folder is packed into Windows exports automatically. On Linux/macOS
the extension has no library, so **exclude it** from those exports — otherwise
Godot logs a "no GDExtension library for this platform" error on startup.

### Windows export
Keep the `gasio/` folder included. The DLL is bundled next to the executable.

### Linux & macOS export

1. Ensure scripts guard ASIO usage with `ClassDB.class_exists(...)` (bundled ones do).
2. **Project → Export…**, select the **Linux** (or **macOS**) preset.
3. **Resources** tab → **"Filters to exclude files/folders…"** → add:
   ```
   gasio/*
   ```
   (Already set in [export_presets.cfg](test_project/export_presets.cfg) as
   `exclude_filter="gasio/*"`.)
4. **Enable Audio Input** in Project Settings so the fallback path works.
5. **macOS only:** for mic access under code signing / sandbox, enable the
   **Audio Input** entitlement and fill in **Privacy → Microphone Usage
   Description**, or a signed build is denied mic access at runtime.
6. Export. The dropdowns show the OS backend and `ASIO: (unavailable on this
   platform)`; the router runs in native mode and detection still works.

> The `exclude_filter` is relative to `res://`. If you move the plugin to
> `res://addons/gasio/`, change it to `addons/gasio/*`.

---

## Building from source

```bash
git submodule update --init --recursive

# Debug (loads in the editor, embeds class docs)
scons platform=windows target=template_debug arch=x86_64 precision=single

# Release (final, for exported games)
scons platform=windows target=template_release arch=x86_64 precision=single
```

The compiled library is installed into `test_project/gasio/bin/windows/`.

### Notes

- **Use MSVC**, not MinGW. The ASIO SDK is MSVC-oriented; MinGW + LTO currently
  fails to link against godot-cpp (`undefined reference to operator new`). With
  MinGW, disable LTO (`lto=none`).
- Only **Windows** sources can be built — the ASIO SDK depends on `windows.h`,
  COM, and the registry.
- CI builds (Windows x86_32 / x86_64 / arm64) come from the GitHub Actions
  workflow; the assembled, shippable `gasio/` folder is uploaded as an artifact.

---

## Credits & license

- This project uses the **Steinberg ASIO SDK**. ASIO is a trademark and software
  of Steinberg Media Technologies GmbH. Use of the ASIO SDK is subject to
  Steinberg's license terms — review them before redistributing binaries.
- See [LICENSE](LICENSE) for this project's license.
