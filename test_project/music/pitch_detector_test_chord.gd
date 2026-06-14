extends Node

## Listens to a chord detector and prints each detected chord.
## Attach to a Node that has the pitch_detector_chord.gd script on a sibling
## or child node, and point `chord_detector_path` at it.

@onready var _detector: Node = $"../pitch_detector"



func _ready() -> void:


	_detector.chord_detected.connect(_on_chord_detected)
	print("pitch_detector_chord_test: listening for chords...")


func _on_chord_detected(chord_event: ChordEvent) -> void:
	# ChordEvent has _to_string(), so this prints e.g.
	#   "E5  (root E2, notes [E, B], -12.3 dB, ch1)"
	print(chord_event)

	# ...or access fields directly:
	# print("Chord: %-5s | Root: %-4s | Quality: %-4s | Notes: [%s] | %5.1f dB | ch%d" % [
	#     chord_event.chord_name, chord_event.root_full, chord_event.quality,
	#     ", ".join(chord_event.notes), chord_event.volume_db, chord_event.channel])
