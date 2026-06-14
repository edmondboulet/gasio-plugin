extends Node

## Listens to a PitchDetector and prints each detected note's info.
## Attach this to a Node that has a PitchDetector as a child named "PitchDetector",
## or set `pitch_detector_path` in the inspector.

@onready var _detector: Node = $"../pitch_detector"





func _ready() -> void:

	_detector.note_detected.connect(_on_note_detected)
	print("pitch_detector_test: listening for notes...")


func _on_note_detected(note_event: NoteEvent) -> void:
	# NoteEvent has _to_string(), so this prints e.g. "E2 (82.6 Hz, -18.3 dB, ch1, -7¢)"
	print(note_event)

	# ...or access fields directly:
	# print("Note: %-3s | Octave: %d | Full: %-4s | Volume: %6.1f dB | Freq: %7.2f Hz | ch%d | %+.0f cents" % [
	#     note_event.note, note_event.octave, note_event.full_name,
	#     note_event.volume_db, note_event.frequency, note_event.channel, note_event.cents])
