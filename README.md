# gasio — Low-latency ASIO audio input for Godot

A Godot 4.6 GDExtension that captures audio from an **ASIO** driver, giving much
lower input latency than the standard OS audio path. Built for real-time uses
like instrument input, tuners, and rhythm/music games.

> **Platform:** the ASIO API is Windows-only. On Linux/macOS the extension is not
> loaded and the game falls back to Godot's built-in audio input (ALSA/PulseAudio
> / CoreAudio). The included pitch/chord detection scripts work on every platform.

---

## Why

Godot's standard microphone input goes through the OS mixer and carries
significant latency. ASIO (Audio Stream Input/Output) talks much closer to the
hardware, which is essential for anything where the player hears or reacts to
their own input in real time — guitar/bass input, tuners, beat-matching, etc.

---

## Platform support

| Platform | ASIO input | Fallback input | Pitch/chord detection |
|----------|:----------:|----------------|:---------------------:|
| Windows  | ✅ ASIO     | WASAPI         | ✅ |
| Linux    | ❌ (absent) | ALSA / PulseAudio | ✅ |
| macOS    | ❌ (absent) | CoreAudio      | ✅ |

On non-Windows platforms `AsioAudioInput` does not exist. Guard any usage with
`ClassDB.class_exists("AsioAudioInput")` (the bundled scripts already do this).

- **Godot version:** 4.6+ (`compatibility_minimum = "4.6"`)

---

## Installation

1. Copy the `gasio/` folder (containing `gasio.gdextension` and `bin/`) into your
   project. It is self-contained — the library paths are relative to the
   `.gdextension` file, so the folder can live anywhere.
2. Restart the Godot editor so the extension loads.

### Project settings prerequisites

- **Project Settings → Audio → Driver → Enable Input** must be ON (required for
  the WASAPI/fallback mic path and the detectors).
- Create a bus named **`Record`** in the Audio panel. The detectors attach their
  capture/analysis effects to this bus automatically.

---

## Demo project

The [`test_project/`](test_project/) folder is a runnable Godot project that
wires everything together. Its main scene
([main.tscn](test_project/main.tscn)) contains:

- An **OptionButton** running [input_selector.gd](test_project/input_selector.gd)
  — the unified WASAPI/ASIO device picker, with a child `AudioStreamPlayer`
  (microphone) routed to the `Record` bus.
- A **pitch_detector** node ([music/pitch_detector.gd](test_project/music/pitch_detector.gd))
  listening to the `Record` bus.
- A **pitch_detector_test** node ([music/pitch_detector_test.gd](test_project/music/pitch_detector_test.gd))
  that prints each detected note to the Output panel.

### Running it

1. Open `test_project/` in Godot 4.6 (it has its own `project.godot`).
2. On first open, let the editor build/load the GDExtension, then reload the
   project if prompted.
3. Confirm the prerequisites: **Audio → Enable Input** is on, and a **`Record`**
   bus exists in the Audio panel.
4. Press **Play**. Use the dropdown (top-left) to pick an input — WASAPI/ASIO on
   Windows, ALSA/Pulse/CoreAudio elsewhere.
5. Make sound into the selected input and watch the **Output** panel: detected
   notes print as `E2 (82.6 Hz, -18.3 dB, ch1, -7¢)`.

To try chord detection instead, swap the detector script to
[music/pitch_detector_chord.gd](test_project/music/pitch_detector_chord.gd) and
the test to [music/pitch_detector_test_chord.gd](test_project/music/pitch_detector_test_chord.gd)
(e.g. play an E power chord → `E5`).

---

## Quick start

```gdscript
func _ready() -> void:
    if not ClassDB.class_exists("AsioAudioInput"):
        return # not on Windows — use the WASAPI/fallback path instead

    var asio := AsioAudioInput.new()
    add_child(asio)

    var drivers := asio.get_driver_list()
    if drivers.is_empty():
        push_warning("No ASIO drivers installed (try ASIO4ALL).")
        return

    asio.audio_frame.connect(_on_audio_frame)
    if asio.open(drivers[0], -1): # -1 = minimum buffer (lowest latency)
        asio.start()

func _on_audio_frame(data: PackedFloat32Array, channel: int) -> void:
    pass # one block of normalized float samples for `channel`
```

See the in-editor class reference for `AsioAudioInput` (generated from
[doc_classes/AsioAudioInput.xml](doc_classes/AsioAudioInput.xml)).

### Buffer size & sample rate (read this)

- `open(name, buffer_size)`: `0` = driver preferred, `-1` = driver minimum
  (lowest latency), any positive value is clamped to the supported range.
- Match your `AudioStreamGenerator.mix_rate` to `asio.get_sample_rate()`, and set
  the ASIO driver's rate to `AudioServer.get_mix_rate()` to avoid resampling
  artifacts and buffer under/overruns.

---

## What's included

The GDExtension exposes one class; the rest are GDScript helpers in
[`test_project/`](test_project/) you can copy into your game:

| File | Purpose |
|------|---------|
| `AsioAudioInput` (C++) | The ASIO capture node. |
| [input_selector.gd](test_project/input_selector.gd) | Unified dropdown listing WASAPI **and** ASIO inputs; switches the live input and routes it to the `Record` bus. |
| [music/pitch_detector.gd](test_project/music/pitch_detector.gd) | Monophonic note detection (YIN), emits `NoteEvent`. |
| [music/pitch_detector_chord.gd](test_project/music/pitch_detector_chord.gd) | Polyphonic chord detection (spectrum), emits `ChordEvent`. |
| [music/note_event.gd](test_project/music/note_event.gd) / [music/chord_event.gd](test_project/music/chord_event.gd) | Value objects for detection results. |
| [music/pitch_detector_test.gd](test_project/music/pitch_detector_test.gd) / [music/pitch_detector_test_chord.gd](test_project/music/pitch_detector_test_chord.gd) | Minimal usage examples that print events. |

Detection range is **E2 (82.41 Hz) – C6 (1046.50 Hz)**. The detectors read the
`Record` bus, so they're independent of the input source (ASIO or fallback).

---

## Latency guide

ASIO lowers *input* latency, but the total round-trip you hear is dominated by
the parts shared with every input path:

```
ASIO capture (~1-3ms)  →  signal hop to main thread (~16ms @60fps)
   →  AudioStreamGenerator buffer  →  Godot output latency (~15ms)  →  speakers
```

To actually reduce perceived latency:

1. **Lower Godot's output latency** — Project Settings → Audio → Output Latency
   (e.g. 15 → 5 ms). Biggest single win; affects all inputs.
2. **Match sample rates** (ASIO ↔ `AudioServer.get_mix_rate()`).
3. **Use a native ASIO interface** (Focusrite/RME/MOTU) rather than ASIO4ALL,
   which is a WASAPI wrapper and has a similar floor to plain WASAPI.
4. For true near-zero monitoring you'd route ASIO-in → ASIO-out directly,
   bypassing Godot's mixer (and losing bus effects on that signal).

---

## Known limitations

- **Windows-only** for ASIO (by design — ASIO doesn't exist elsewhere).
- **ASIO4ALL exclusive-mode contention:** if Godot's WASAPI input holds the same
  device ASIO4ALL wraps, ASIO can't reacquire it. `input_selector.gd` works
  around this by "parking" Godot's input on another device.
- **Chord detection** is approximate: harmonics can add phantom notes (raise
  `presence_ratio`), and low-note resolution is limited even at FFT 4096.
- **`double` precision is not supported** — godot-cpp ships a single-precision
  `extension_api.json`.

---

## Exporting your game

The `gasio/` folder is packed into the export automatically on Windows. On
Linux/macOS the extension has no library, so you must **exclude it** from those
exports — otherwise Godot logs a "no GDExtension library for this platform"
error on startup.

### Windows export
Nothing special — keep the `gasio/` folder included. The DLL is bundled next to
the executable by the exporter.

### Linux & macOS export

1. Make sure all your scripts guard ASIO usage with
   `ClassDB.class_exists("AsioAudioInput")` (the bundled scripts do).
2. In **Project → Export…**, select the **Linux** (or **macOS**) preset.
3. Open the **Resources** tab → **"Filters to exclude files/folders…"** and add:
   ```
   gasio/*
   ```
   This stops the extension and its `bin/` from being packaged, so Godot never
   tries to load a non-existent library. (Already set in
   [export_presets.cfg](test_project/export_presets.cfg) as
   `exclude_filter="gasio/*"`.)
4. **Enable Audio Input** in Project Settings so the fallback mic path works.
5. **macOS only:** for microphone access under code signing / sandbox, set
   - Export preset → *Codesign* → enable the **Audio Input** entitlement, and
   - fill in **Privacy → Microphone Usage Description**.
   Without these, a signed/sandboxed macOS build is denied mic access at runtime.
6. Export. On these platforms the input dropdown shows the OS backend
   (ALSA/Pulse, CoreAudio) and `ASIO: (unavailable on this platform)`; the
   detectors keep working off the `Record` bus.

> If the `exclude_filter` doesn't match, check the folder path — the filter is
> relative to `res://`. If you move the plugin to `res://addons/gasio/`, change
> the filter to `addons/gasio/*`.

---

## Building from source

```bash
git submodule update --init --recursive

# Debug (loads in the editor)
scons platform=windows target=template_debug arch=x86_64 precision=single

# Release (final, for exported games)
scons platform=windows target=template_release arch=x86_64 precision=single
```

The compiled library is installed into `test_project/gasio/bin/windows/`.

### Notes

- **Use MSVC**, not MinGW. The ASIO SDK is MSVC-oriented; MinGW + LTO currently
  fails to link against godot-cpp (`undefined reference to operator new`). If you
  build with MinGW, disable LTO (`lto=none`).
- Only **Windows** sources can be built — the ASIO SDK depends on `windows.h`,
  COM, and the registry.
- CI builds (Windows x86_32 / x86_64 / arm64) are produced by the GitHub Actions
  workflow; the assembled, shippable `gasio/` folder is uploaded as an artifact.

---

## Credits & license

- This project uses the **Steinberg ASIO SDK**. ASIO is a trademark and software
  of Steinberg Media Technologies GmbH. Your use of the ASIO SDK is subject to
  Steinberg's license terms — review them before redistributing binaries.
- See [LICENSE](LICENSE) for this project's license.
