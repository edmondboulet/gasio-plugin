extends OptionButton

## Unified audio input selector.
## Lists every WASAPI input device AND every installed ASIO driver in one
## dropdown. Picking an entry switches the live input and routes it to the
## 'Record' bus. Attach this to an OptionButton node.

@export var record_bus: String = "Record"
@export var asio_buffer_size: int = -1   # -1 = driver minimum (lowest latency)

## Godot always keeps ONE WASAPI input device open. If that device is the same
## one ASIO4ALL wraps, ASIO can't acquire it back after a WASAPI session.
## Before opening ASIO we "park" Godot's input on this device to release the
## contended one. Set it to a device ASIO does NOT use (e.g. your Yeti).
## Leave empty to park on "Default".
@export var asio_park_device: String = ""

# Item metadata kinds
enum Kind { WASAPI, ASIO }

# --- WASAPI playback (Godot's own microphone capture) ---
var _mic_player: AudioStreamPlayer

# Muted mic stream kept alive while ASIO runs, capturing the "park" device.
# This forces Godot to actually hold the park device (releasing the one ASIO
# needs). set_input_device() alone is lazy and won't release until capturing.
var _park_player: AudioStreamPlayer

# --- ASIO playback (our GDExtension) ---
# Untyped on purpose: the AsioAudioInput class only exists on Windows (where the
# GDExtension is loaded). Referencing the type name statically would fail to
# PARSE on Linux/macOS, breaking the whole script. We instantiate it via
# ClassDB and call its methods dynamically instead.
var _asio = null
var _asio_player: AudioStreamPlayer
var _asio_playback: AudioStreamGeneratorPlayback
var _asio_ch0: PackedFloat32Array
var _asio_ch1: PackedFloat32Array
var _asio_stereo: bool = false

# Serializes device switches so quick back-and-forth selections don't overlap.
# ASIO4ALL / Rocksmith-style devices are opened in exclusive mode, so the
# previous input MUST be fully released before the next one is opened.
var _switching: bool = false


func _ready() -> void:
	# Let Godot's audio driver settle before querying devices
	await get_tree().create_timer(0.1).timeout

	_populate()
	item_selected.connect(_on_item_selected)


func _populate() -> void:
	clear()
	var id := 0

	# --- System audio input section (backend name depends on OS) ---
	var backend := _input_backend_label()
	add_separator("%s Inputs" % backend)
	for dev in AudioServer.get_input_device_list():
		add_item("%s: %s" % [backend, dev], id)
		set_item_metadata(get_item_index(id), {"kind": Kind.WASAPI, "name": dev})
		id += 1

	# --- ASIO section ---
	add_separator("ASIO Drivers")
	if not _asio_supported():
		# Not Windows (or extension not loaded) — show a disabled placeholder.
		add_item("ASIO: (unavailable on this platform)", id)
		set_item_disabled(get_item_index(id), true)
		id += 1
		return

	var asio_probe = ClassDB.instantiate("AsioAudioInput")
	add_child(asio_probe)
	var drivers: Array = asio_probe.get_driver_list()
	for d in drivers:
		add_item("ASIO: " + str(d), id)
		set_item_metadata(get_item_index(id), {"kind": Kind.ASIO, "name": str(d)})
		id += 1
	asio_probe.queue_free()

	if drivers.is_empty():
		add_item("ASIO: (none installed)", id)
		set_item_disabled(get_item_index(id), true)
		id += 1


# True only where the GDExtension is loaded (Windows).
func _asio_supported() -> bool:
	return ClassDB.class_exists("AsioAudioInput")


# Name of the OS audio backend that AudioServer's input devices come from.
# Purely cosmetic — the capture itself works the same on every platform.
func _input_backend_label() -> String:
	match OS.get_name():
		"Windows":
			return "WASAPI"
		"macOS":
			return "CoreAudio"
		"Linux", "FreeBSD", "NetBSD", "OpenBSD", "BSD":
			return "ALSA/Pulse"
		_:
			return "Audio In"


func _on_item_selected(index: int) -> void:
	var meta = get_item_metadata(index)
	if meta == null:
		return  # separator or disabled row

	if _switching:
		print("[Input] switch already in progress, ignoring.")
		return
	_switching = true

	# Tear everything down and WAIT for the device to be fully released before
	# opening the next one. Exclusive-mode devices fail to open otherwise.
	_stop_wasapi()
	_stop_asio()
	# One frame lets queue_free() actually free the old ASIO node + its driver.
	await get_tree().process_frame
	await get_tree().create_timer(0.25).timeout

	match meta["kind"]:
		Kind.WASAPI:
			await _start_wasapi(meta["name"])
		Kind.ASIO:
			# Release the WASAPI device Godot holds so ASIO4ALL can grab it.
			await _park_godot_input()
			_start_asio(meta["name"])

	_switching = false


# ---------------------------------------------------------------------------
# WASAPI
# ---------------------------------------------------------------------------

func _start_wasapi(device: String) -> void:
	# Set the device first, give the driver time to switch, THEN start capture.
	AudioServer.set_input_device(device)
	await get_tree().create_timer(0.3).timeout

	# Verify the switch took; retry once if the driver didn't apply it.
	if AudioServer.get_input_device() != device:
		AudioServer.set_input_device(device)
		await get_tree().create_timer(0.3).timeout

	# Recreate the mic player fresh each time — reusing a stopped
	# AudioStreamMicrophone after a device change can stay silent.
	_mic_player = AudioStreamPlayer.new()
	_mic_player.stream = AudioStreamMicrophone.new()
	_mic_player.bus = record_bus
	add_child(_mic_player)
	_mic_player.play()

	if AudioServer.get_input_device() == device:
		print("[Input] WASAPI active: ", AudioServer.get_input_device())
	else:
		push_warning("[Input] WASAPI device '%s' did not apply (got '%s'). It may be held exclusively by another app or ASIO." % [
			device, AudioServer.get_input_device()])


func _stop_wasapi() -> void:
	if _mic_player:
		_mic_player.stop()
		_mic_player.queue_free()
		_mic_player = null


# Force Godot to actively capture the park device so it releases the device
# ASIO needs. A silent (-80 dB) mic stream is kept alive for the whole ASIO
# session; freeing it would let Godot drift back onto the contended device.
func _park_godot_input() -> void:
	var park := asio_park_device if asio_park_device != "" else "Default"
	AudioServer.set_input_device(park)

	_park_player = AudioStreamPlayer.new()
	_park_player.stream = AudioStreamMicrophone.new()
	_park_player.bus = record_bus
	_park_player.volume_db = -80.0   # inaudible; only here to hold the device open
	add_child(_park_player)
	_park_player.play()

	print("[Input] parked Godot input on '%s' (muted) to free device for ASIO." % park)
	await get_tree().create_timer(0.3).timeout


func _stop_park() -> void:
	if _park_player:
		_park_player.stop()
		_park_player.queue_free()
		_park_player = null


# ---------------------------------------------------------------------------
# ASIO
# ---------------------------------------------------------------------------

func _start_asio(driver_name: String) -> void:
	if not _asio_supported():
		push_error("[Input] ASIO is not available on this platform.")
		return

	_asio = ClassDB.instantiate("AsioAudioInput")
	add_child(_asio)

	if not _asio.open(driver_name, asio_buffer_size):
		push_error("[Input] Failed to open ASIO driver '%s'." % driver_name)
		_stop_asio()
		return

	_asio_stereo = _asio.get_input_channels() >= 2

	var gen := AudioStreamGenerator.new()
	var sample_rate := float(_asio.get_sample_rate())
	gen.mix_rate = sample_rate
	var block_based := (float(_asio.get_buffer_size()) / sample_rate) * 16.0
	gen.buffer_length = maxf(block_based, 0.1)

	_asio_player = AudioStreamPlayer.new()
	_asio_player.stream = gen
	_asio_player.bus = record_bus
	add_child(_asio_player)
	_asio_player.play()
	_asio_playback = _asio_player.get_stream_playback() as AudioStreamGeneratorPlayback

	_asio.audio_frame.connect(_on_asio_frame)

	if not _asio.start():
		push_error("[Input] ASIOStart() failed.")
		_stop_asio()
		return

	print("[Input] ASIO active: %s  |  %.0f Hz  |  buf %d  |  %d ch" % [
		driver_name, _asio.get_sample_rate(), _asio.get_buffer_size(), _asio.get_input_channels()])


func _stop_asio() -> void:
	if _asio:
		if _asio.audio_frame.is_connected(_on_asio_frame):
			_asio.audio_frame.disconnect(_on_asio_frame)
		_asio.close()
		_asio.queue_free()
		_asio = null
	if _asio_player:
		_asio_player.queue_free()
		_asio_player = null
	_asio_playback = null
	_asio_ch0 = PackedFloat32Array()
	_asio_ch1 = PackedFloat32Array()
	# Release the muted park holder too
	_stop_park()


func _on_asio_frame(data: PackedFloat32Array, channel: int) -> void:
	if channel == 0:
		_asio_ch0 = data
		if not _asio_stereo:
			_push_asio(_asio_ch0, _asio_ch0)
	elif channel == 1:
		_asio_ch1 = data
		if not _asio_ch0.is_empty():
			_push_asio(_asio_ch0, _asio_ch1)


func _push_asio(left: PackedFloat32Array, right: PackedFloat32Array) -> void:
	if not _asio_playback:
		return
	var buf_size := left.size()
	if _asio_playback.get_frames_available() < buf_size:
		_asio_ch0 = PackedFloat32Array()
		_asio_ch1 = PackedFloat32Array()
		return
	var frames := PackedVector2Array()
	frames.resize(buf_size)
	for i in buf_size:
		frames[i] = Vector2(left[i], right[i])
	_asio_playback.push_buffer(frames)
	_asio_ch0 = PackedFloat32Array()
	_asio_ch1 = PackedFloat32Array()


func _exit_tree() -> void:
	_stop_asio()
