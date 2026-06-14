extends Node

## Listens to an audio bus and emits the musical note being played, per channel.
## Monophonic per channel — designed for guitar/bass/voice in the
## range E2 (82.41 Hz) to C6 (1046.50 Hz).
##
## Attach to any Node. It adds an AudioEffectCapture to `bus_name` at runtime,
## pulls raw samples, and runs YIN pitch detection on them.
##
## Channel handling:
##   check_both_channels = false -> only channel 1 (left) is analysed.
##   check_both_channels = true  -> alternates channel 1 / 2 each tick, so both
##                                  are covered without doubling the per-tick cost.

## Emitted whenever a valid in-range note is detected above the volume gate.
## Carries a NoteEvent (see note_event.gd); its `channel` field is 1 or 2.
signal note_detected(event: NoteEvent)

@export var bus_name: String = "Record"
@export var min_volume_db: float = -45.0   # gate: below this, nothing is emitted
@export var analysis_size: int = 2048      # samples per detection (bigger = better low notes, more CPU)
@export var emit_only_on_change: bool = false  # avoid spamming the same note every frame
@export var check_both_channels: bool = false # false = channel 1 only; true = alternate 1/2 each tick

# Note range limits (MIDI numbers). E2 = 40, C6 = 84.
const MIDI_MIN := 40
const MIDI_MAX := 84
const NOTE_NAMES := ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Search a little beyond the note range so edge notes still resolve.
const FREQ_MIN := 78.0     # just below E2
const FREQ_MAX := 1100.0   # just above C6
const YIN_THRESHOLD := 0.15

var _capture: AudioEffectCapture
var _sample_rate: float = 48000.0

# One accumulation buffer per channel so each analysis window is contiguous.
var _accum_ch1: PackedFloat32Array = PackedFloat32Array()
var _accum_ch2: PackedFloat32Array = PackedFloat32Array()

# Last note emitted per channel, for emit_only_on_change.
var _last_full_note := {1: "", 2: ""}

# Which channel to analyse on the next tick (used when check_both_channels).
var _current_channel: int = 1


func _ready() -> void:
	_sample_rate = AudioServer.get_mix_rate()

	var bus_idx := AudioServer.get_bus_index(bus_name)
	if bus_idx < 0:
		push_error("PitchDetector: bus '%s' not found." % bus_name)
		set_process(false)
		return

	# Reuse an existing capture effect if present, otherwise add one.
	for i in AudioServer.get_bus_effect_count(bus_idx):
		var fx := AudioServer.get_bus_effect(bus_idx, i)
		if fx is AudioEffectCapture:
			_capture = fx
			break
	if _capture == null:
		_capture = AudioEffectCapture.new()
		AudioServer.add_bus_effect(bus_idx, _capture)

	print("PitchDetector: listening on bus '%s' @ %.0f Hz" % [bus_name, _sample_rate])


func _process(_delta: float) -> void:
	if _capture == null:
		return

	var available := _capture.get_frames_available()
	if available <= 0:
		return

	# Pull stereo frames and keep each channel separate.
	var stereo := _capture.get_buffer(available)
	var start1 := _accum_ch1.size()
	var start2 := _accum_ch2.size()
	_accum_ch1.resize(start1 + stereo.size())
	_accum_ch2.resize(start2 + stereo.size())
	for i in stereo.size():
		_accum_ch1[start1 + i] = stereo[i].x   # channel 1 = left
		_accum_ch2[start2 + i] = stereo[i].y   # channel 2 = right

	# Decide which channel(s) to analyse this tick.
	if check_both_channels:
		# Alternate the channel every tick.
		_try_analyze(_current_channel)
		_current_channel = 2 if _current_channel == 1 else 1
	else:
		_try_analyze(1)

	# Cap buffer growth so unanalysed channels don't grow without bound.
	_trim(1)
	_trim(2)


func _try_analyze(channel: int) -> void:
	var buf := _accum_ch1 if channel == 1 else _accum_ch2
	if buf.size() < analysis_size:
		return

	var window := buf.slice(buf.size() - analysis_size)
	# Reset the analysed channel's buffer; acts as a natural throttle.
	if channel == 1:
		_accum_ch1 = PackedFloat32Array()
	else:
		_accum_ch2 = PackedFloat32Array()

	_analyze(window, channel)


func _trim(channel: int) -> void:
	# Keep at most two windows of history per channel.
	var cap := analysis_size * 2
	if channel == 1 and _accum_ch1.size() > cap:
		_accum_ch1 = _accum_ch1.slice(_accum_ch1.size() - analysis_size)
	elif channel == 2 and _accum_ch2.size() > cap:
		_accum_ch2 = _accum_ch2.slice(_accum_ch2.size() - analysis_size)


func _analyze(buf: PackedFloat32Array, channel: int) -> void:
	# Volume gate
	var rms := 0.0
	for s in buf:
		rms += s * s
	rms = sqrt(rms / buf.size())
	var volume_db := linear_to_db(rms) if rms > 0.0 else -INF

	if volume_db < min_volume_db:
		_last_full_note[channel] = ""   # treat as silence so the next note re-emits
		return

	var freq := _detect_frequency(buf)
	if freq <= 0.0:
		return

	# Frequency → MIDI → note
	var midi := 69.0 + 12.0 * (log(freq / 440.0) / log(2.0))
	var midi_round := int(round(midi))

	# Enforce the E2..C6 range
	if midi_round < MIDI_MIN or midi_round > MIDI_MAX:
		return

	var note: String = NOTE_NAMES[midi_round % 12]
	var octave := int(midi_round / 12) - 1
	var full_note := "%s%d" % [note, octave]

	if emit_only_on_change and full_note == _last_full_note[channel]:
		return
	_last_full_note[channel] = full_note

	var cents := (midi - midi_round) * 100.0
	note_detected.emit(NoteEvent.new(note, octave, full_note, freq, volume_db, channel, midi_round, cents))


# YIN pitch detection (de Cheveigné & Kawahara, 2002) — robust against the
# octave errors that plain autocorrelation makes.
func _detect_frequency(buf: PackedFloat32Array) -> float:
	var size := buf.size()
	var min_tau := int(_sample_rate / FREQ_MAX)
	var max_tau := int(_sample_rate / FREQ_MIN)
	if max_tau >= size:
		max_tau = size - 1
	if min_tau < 1:
		min_tau = 1

	# 1. Difference function
	var diff := PackedFloat32Array()
	diff.resize(max_tau + 1)
	var win := size - max_tau
	for tau in range(1, max_tau + 1):
		var sum := 0.0
		for i in range(win):
			var d := buf[i] - buf[i + tau]
			sum += d * d
		diff[tau] = sum

	# 2. Cumulative mean normalized difference
	var cmnd := PackedFloat32Array()
	cmnd.resize(max_tau + 1)
	cmnd[0] = 1.0
	var running := 0.0
	for tau in range(1, max_tau + 1):
		running += diff[tau]
		cmnd[tau] = diff[tau] * tau / running if running > 0.0 else 1.0

	# 3. Absolute threshold — first dip below threshold, refined to local min
	var tau_found := -1
	var tau := min_tau
	while tau <= max_tau:
		if cmnd[tau] < YIN_THRESHOLD:
			while tau + 1 <= max_tau and cmnd[tau + 1] < cmnd[tau]:
				tau += 1
			tau_found = tau
			break
		tau += 1

	if tau_found == -1:
		return 0.0

	# 4. Parabolic interpolation for sub-sample precision
	var better := float(tau_found)
	if tau_found > min_tau and tau_found < max_tau:
		var s0 := cmnd[tau_found - 1]
		var s1 := cmnd[tau_found]
		var s2 := cmnd[tau_found + 1]
		var denom := 2.0 * (2.0 * s1 - s2 - s0)
		if absf(denom) > 0.00001:
			better = tau_found + (s2 - s0) / denom

	return _sample_rate / better
