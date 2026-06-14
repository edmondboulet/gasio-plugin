class_name ChordEvent
extends RefCounted

## A detected chord, emitted by pitch_detector_chord.gd.

var chord_name: String   # full chord name, e.g. "E5", "C", "Am", "G7"
var quality: String      # quality suffix only, e.g. "5", "", "m", "7"

var root: String         # root note letter + accidental, e.g. "E", "C#"
var root_octave: int     # octave of the root note, e.g. 2
var root_full: String    # root note + octave, e.g. "E2"
var root_midi: int       # MIDI number of the root, e.g. 40 = E2
var root_frequency: float # frequency of the root note, in Hz

var notes: PackedStringArray # all pitch classes detected, e.g. ["E", "B"]
var volume_db: float     # loudest band level, in dBFS
var channel: int         # source channel (1 = left, 2 = right)


func _init(p_chord_name: String = "", p_quality: String = "", p_root: String = "",
		p_root_octave: int = 0, p_root_full: String = "", p_root_midi: int = 0,
		p_root_frequency: float = 0.0, p_notes: PackedStringArray = PackedStringArray(),
		p_volume_db: float = 0.0, p_channel: int = 1) -> void:
	chord_name = p_chord_name
	quality = p_quality
	root = p_root
	root_octave = p_root_octave
	root_full = p_root_full
	root_midi = p_root_midi
	root_frequency = p_root_frequency
	notes = p_notes
	volume_db = p_volume_db
	channel = p_channel


func _to_string() -> String:
	return "%s  (root %s, notes [%s], %.1f dB, ch%d)" % [
		chord_name, root_full, ", ".join(notes), volume_db, channel]
