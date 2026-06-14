class_name NoteEvent
extends RefCounted

## A single detected musical note, emitted by the pitch detectors.

var note: String      # letter + accidental, e.g. "E", "C#"
var octave: int       # octave number, e.g. 2
var full_name: String # note + octave, e.g. "E2", "C#4"
var frequency: float  # detected fundamental, in Hz
var volume_db: float  # signal level, in dBFS
var channel: int      # source channel (1 = left, 2 = right)
var midi: int         # MIDI note number, e.g. 40 = E2
var cents: float      # offset from perfect pitch, in cents (−50..+50)


func _init(p_note: String = "", p_octave: int = 0, p_full_name: String = "",
		p_frequency: float = 0.0, p_volume_db: float = 0.0, p_channel: int = 1,
		p_midi: int = 0, p_cents: float = 0.0) -> void:
	note = p_note
	octave = p_octave
	full_name = p_full_name
	frequency = p_frequency
	volume_db = p_volume_db
	channel = p_channel
	midi = p_midi
	cents = p_cents


func _to_string() -> String:
	return "%s (%.1f Hz, %.1f dB, ch%d, %+.0f¢)" % [
		full_name, frequency, volume_db, channel, cents]
