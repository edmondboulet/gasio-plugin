extends Node

## Listens to an audio bus and emits the CHORD being played (root note + chord
## name). Polyphonic — unlike the YIN detectors, this uses a spectrum analyser
## so it can see several notes at once.
##
## Examples:
##   E2 + B2 + E3  (E power chord)  -> "E5"
##   C  + E  + G   (C major)        -> "C"
##   A  + C  + E   (A minor)        -> "Am"
##
## Attach to any Node. It adds an AudioEffectSpectrumAnalyzer to `bus_name` at
## runtime and queries the magnitude of every semitone in the E2..C6 range.
##
## Channel handling mirrors pitch_detector_channel.gd:
##   check_both_channels = false -> only channel 1 (left).
##   check_both_channels = true  -> alternates channel 1 / 2 each tick.

## Carries a ChordEvent (see chord_event.gd).
signal chord_detected(event: ChordEvent)

@export var bus_name: String = "Record"
@export var min_volume_db: float = -45.0      # gate: below this, nothing is emitted
@export var emit_only_on_change: bool = true  # only emit when the chord name changes
@export var check_both_channels: bool = false # false = channel 1 only; true = alternate 1/2

## A semitone counts as "present" if its magnitude is at least this fraction of
## the loudest band. Higher = stricter (fewer false notes from harmonics/noise).
@export_range(0.05, 0.9, 0.01) var presence_ratio: float = 0.30

# Note range (MIDI). E2 = 40, C6 = 84.
const MIDI_MIN := 40
const MIDI_MAX := 84
const NOTE_NAMES := ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Chord templates: intervals (semitones from root) -> quality suffix.
# Longer/more-specific templates are tried first so e.g. a major7 isn't
# mistaken for a plain major.
const CHORD_TEMPLATES := [
	{"intervals": [0, 4, 7, 11], "suffix": "maj7"},
	{"intervals": [0, 3, 7, 10], "suffix": "m7"},
	{"intervals": [0, 4, 7, 10], "suffix": "7"},
	{"intervals": [0, 4, 7, 9],  "suffix": "6"},
	{"intervals": [0, 3, 7, 9],  "suffix": "m6"},
	{"intervals": [0, 4, 7],     "suffix": ""},     # major
	{"intervals": [0, 3, 7],     "suffix": "m"},    # minor
	{"intervals": [0, 3, 6],     "suffix": "dim"},
	{"intervals": [0, 4, 8],     "suffix": "aug"},
	{"intervals": [0, 2, 7],     "suffix": "sus2"},
	{"intervals": [0, 5, 7],     "suffix": "sus4"},
	{"intervals": [0, 7],        "suffix": "5"},    # power chord
]

var _spectrum: AudioEffectSpectrumAnalyzerInstance
var _sample_rate: float = 48000.0
var _last_chord := {1: "", 2: ""}
var _current_channel: int = 1


func _ready() -> void:
	_sample_rate = AudioServer.get_mix_rate()

	var bus_idx := AudioServer.get_bus_index(bus_name)
	if bus_idx < 0:
		push_error("ChordDetector: bus '%s' not found." % bus_name)
		set_process(false)
		return

	# Find an existing spectrum analyser on the bus, or add one.
	var fx_idx := -1
	for i in AudioServer.get_bus_effect_count(bus_idx):
		if AudioServer.get_bus_effect(bus_idx, i) is AudioEffectSpectrumAnalyzer:
			fx_idx = i
			break
	if fx_idx == -1:
		var analyzer := AudioEffectSpectrumAnalyzer.new()
		# Largest FFT = best resolution at the low end (E2 is only ~82 Hz).
		analyzer.fft_size = AudioEffectSpectrumAnalyzer.FFT_SIZE_4096
		AudioServer.add_bus_effect(bus_idx, analyzer)
		fx_idx = AudioServer.get_bus_effect_count(bus_idx) - 1

	_spectrum = AudioServer.get_bus_effect_instance(bus_idx, fx_idx)
	if _spectrum == null:
		push_error("ChordDetector: could not get spectrum analyser instance.")
		set_process(false)
		return

	print("ChordDetector: listening on bus '%s' @ %.0f Hz" % [bus_name, _sample_rate])


func _process(_delta: float) -> void:
	if _spectrum == null:
		return

	if check_both_channels:
		_analyze(_current_channel)
		_current_channel = 2 if _current_channel == 1 else 1
	else:
		_analyze(1)


func _analyze(channel: int) -> void:
	# 1. Measure the magnitude of every semitone in range for this channel.
	var mags: Array[float] = []
	mags.resize(MIDI_MAX - MIDI_MIN + 1)
	var peak := 0.0
	for midi in range(MIDI_MIN, MIDI_MAX + 1):
		var f := _midi_to_freq(midi)
		# Band spanning half a semitone either side of the note.
		var f_lo := f * 0.97153   # 2^(-0.5/12)
		var f_hi := f * 1.02930   # 2^( 0.5/12)
		var v := _spectrum.get_magnitude_for_frequency_range(
			f_lo, f_hi, AudioEffectSpectrumAnalyzerInstance.MAGNITUDE_MAX)
		var m: float = v.x if channel == 1 else v.y
		mags[midi - MIDI_MIN] = m
		if m > peak:
			peak = m

	# 2. Volume gate
	var volume_db := linear_to_db(peak) if peak > 0.0 else -INF
	if volume_db < min_volume_db:
		_last_chord[channel] = ""
		return

	# 3. Collect notes loud enough to count as "present".
	var present_midis: Array[int] = []
	var floor_mag := peak * presence_ratio
	for midi in range(MIDI_MIN, MIDI_MAX + 1):
		if mags[midi - MIDI_MIN] >= floor_mag:
			present_midis.append(midi)

	if present_midis.is_empty():
		return

	# 4. Root = lowest present note. Build the set of pitch classes.
	var root_midi: int = present_midis[0]
	var pitch_classes := {}
	for m in present_midis:
		pitch_classes[m % 12] = true

	# 5. Match a chord template relative to the root's pitch class.
	var root_pc := root_midi % 12
	var intervals := []
	for pc in pitch_classes.keys():
		intervals.append((int(pc) - root_pc + 12) % 12)
	intervals.sort()

	var quality := _match_chord(intervals)

	# Build the friendly note list (unique pitch classes, ordered low->high)
	var note_list := PackedStringArray()
	for m in present_midis:
		var name: String = NOTE_NAMES[m % 12]
		if not note_list.has(name):
			note_list.append(name)

	var root_note: String = NOTE_NAMES[root_pc]
	var root_octave := int(root_midi / 12) - 1
	var root_full := "%s%d" % [root_note, root_octave]
	var chord_name := root_note + quality

	# 6. De-dupe identical consecutive chords per channel.
	if emit_only_on_change and chord_name == _last_chord[channel]:
		return
	_last_chord[channel] = chord_name

	chord_detected.emit(ChordEvent.new(
		chord_name, quality, root_note, root_octave, root_full, root_midi,
		_midi_to_freq(root_midi), note_list, volume_db, channel))


# Returns the chord quality suffix for a sorted interval set, or "" by default.
func _match_chord(intervals: Array) -> String:
	# Single note: no chord quality.
	if intervals.size() == 1:
		return ""

	# Exact-match the templates (longest/most-specific first).
	for tpl in CHORD_TEMPLATES:
		if _intervals_match(intervals, tpl["intervals"]):
			return tpl["suffix"]

	# Fallback: if it contains a perfect fifth but no recognised third,
	# treat it as a power chord.
	if intervals.has(7) and not intervals.has(3) and not intervals.has(4):
		return "5"

	# Unknown shape — report just the root note.
	return ""


# True if `got` contains exactly the template intervals (order-independent).
func _intervals_match(got: Array, template: Array) -> bool:
	if got.size() != template.size():
		return false
	for iv in template:
		if not got.has(iv):
			return false
	return true


func _midi_to_freq(midi: int) -> float:
	return 440.0 * pow(2.0, (midi - 69) / 12.0)
