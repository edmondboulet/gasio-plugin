extends Node
## Autoload singleton coordinating audio routing between the input and output
## selectors.
##
## The selectors don't open drivers themselves — they report the user's choice
## here via request_input()/request_output(), and this router reconciles them:
##
##   input ASIO + output ASIO (same driver) -> ONE full-duplex AsioAudioDevice
##       (direct low-latency monitoring + game audio out the same interface)
##   input ASIO only   -> AsioAudioInput, fed into the Record bus
##   output ASIO only  -> AsioAudioOutput, fed from a capture on Master
##   otherwise         -> Godot's native input/output devices
##
## This is required because ASIO allows only ONE open driver per process; two
## independent selectors grabbing ASIO would collide.

const RECORD_BUS := "Record"

## In full-duplex, fold the monitored input to mono (both ears). Set false to
## keep input channels mapped to L/R separately.
@export var full_duplex_mono := false

## Noise gate on the full-duplex monitor — fades the input out below the
## threshold to remove constant hiss/static.
@export var full_duplex_noise_gate := true
## Gate threshold in dBFS. Signal below this is muted; raise toward 0 if hiss
## still bleeds through, lower if quiet playing gets cut off.
@export var full_duplex_gate_threshold_db := -30.0

# kind: "native" | "asio"
var _in_kind := "native"
var _in_name := ""
var _out_kind := "native"
var _out_name := ""

# Active driver objects (untyped — the classes are Windows-only).
var _device = null   # AsioAudioDevice (full-duplex)
var _asio_in = null  # AsioAudioInput
var _asio_out = null # AsioAudioOutput

# Native input capture -> Record bus
var _mic_player: AudioStreamPlayer

# ASIO input -> Record bus (so the pitch/chord detectors keep working)
var _in_gen_player: AudioStreamPlayer
var _in_playback: AudioStreamGeneratorPlayback
var _in_ch0: PackedFloat32Array
var _in_ch1: PackedFloat32Array
var _in_stereo := false

# Master capture -> ASIO output sink
var _out_capture: AudioEffectCapture

var asio_supported := false


func _ready() -> void:
	asio_supported = ClassDB.class_exists("AsioAudioDevice")
	set_process(false)


# --- public API, called by the selectors -----------------------------------

var _reconciling := false
var _dirty := false

func request_input(kind: String, name: String) -> void:
	_in_kind = kind
	_in_name = name
	_schedule()

func request_output(kind: String, name: String) -> void:
	_out_kind = kind
	_out_name = name
	_schedule()

# Serializes reconciliation so rapid back-and-forth selections don't overlap
# (each one tears down and reopens drivers, which must not interleave).
func _schedule() -> void:
	_dirty = true
	if _reconciling:
		return
	_reconciling = true
	while _dirty:
		_dirty = false
		await _reconcile()
	_reconciling = false


# --- reconciliation ---------------------------------------------------------

func _reconcile() -> void:
	_teardown()
	# Let the previous device fully release before opening the next one.
	await get_tree().create_timer(0.2).timeout

	var both_asio := _in_kind == "asio" and _out_kind == "asio"
	if both_asio and asio_supported:
		if _in_name != _out_name:
			push_warning("[AudioRouter] Full-duplex needs ONE interface; using input driver '%s' for both." % _in_name)
		_build_full_duplex(_in_name)
		return

	# Independent paths
	if _in_kind == "asio" and asio_supported:
		_build_asio_input(_in_name)
	else:
		await _build_native_input(_in_name)

	if _out_kind == "asio" and asio_supported:
		_build_asio_output(_out_name)
	else:
		_build_native_output(_out_name)


# --- full-duplex (one AsioAudioDevice) --------------------------------------

func _build_full_duplex(driver: String) -> void:
	_device = ClassDB.instantiate("AsioAudioDevice")
	add_child(_device)
	if not _device.open(driver, 0):
		push_error("[AudioRouter] Failed to open ASIO device '%s'." % driver)
		_teardown()
		return
	_device.set_direct_monitor(true)   # hear input through output, ASIO-thread latency
	_device.set_monitor_gain(1.0)
	_device.set_mono_input(full_duplex_mono)  # instrument on one input -> both ears
	_device.set_noise_gate(full_duplex_noise_gate)
	_device.set_noise_gate_threshold_db(full_duplex_gate_threshold_db)

	# Feed the device's input into the Record bus so the pitch/chord detectors
	# (which read Record) work in full-duplex too. Mute the Record bus so this
	# copy does NOT leak into the Master/game capture (which would double the
	# input on the output, on top of the direct monitor). AudioEffectCapture
	# reads the signal pre-mute, so detection on Record still works.
	_in_stereo = _device.get_input_channels() >= 2
	_setup_input_generator(_device.get_sample_rate(), _device.get_buffer_size())
	_device.audio_frame.connect(_on_asio_input_frame)
	var rec := AudioServer.get_bus_index(RECORD_BUS)
	if rec >= 0:
		AudioServer.set_bus_mute(rec, true)

	# Send Godot's game audio out the same interface; mute the OS path.
	_out_capture = _ensure_capture("Master")
	AudioServer.set_bus_mute(AudioServer.get_bus_index("Master"), true)

	if not _device.start():
		push_error("[AudioRouter] AsioAudioDevice start failed.")
		_teardown()
		return
	set_process(true)
	print("[AudioRouter] Full-duplex ASIO on '%s' (%.0f Hz, %d in / %d out)" % [
		driver, _device.get_sample_rate(), _device.get_input_channels(), _device.get_output_channels()])


# --- ASIO input only --------------------------------------------------------

func _build_asio_input(driver: String) -> void:
	_asio_in = ClassDB.instantiate("AsioAudioInput")
	add_child(_asio_in)
	if not _asio_in.open(driver, -1):
		push_error("[AudioRouter] Failed to open ASIO input '%s'." % driver)
		_teardown()
		return
	_in_stereo = _asio_in.get_input_channels() >= 2
	_setup_input_generator(_asio_in.get_sample_rate(), _asio_in.get_buffer_size())
	_asio_in.audio_frame.connect(_on_asio_input_frame)
	if not _asio_in.start():
		push_error("[AudioRouter] ASIO input start failed.")
		_teardown()
		return
	print("[AudioRouter] ASIO input on '%s'." % driver)


# --- ASIO output only -------------------------------------------------------

func _build_asio_output(driver: String) -> void:
	_asio_out = ClassDB.instantiate("AsioAudioOutput")
	add_child(_asio_out)
	if not _asio_out.open(driver, 0):
		push_error("[AudioRouter] Failed to open ASIO output '%s'." % driver)
		_teardown()
		return
	_out_capture = _ensure_capture("Master")
	AudioServer.set_bus_mute(AudioServer.get_bus_index("Master"), true)
	if not _asio_out.start():
		push_error("[AudioRouter] ASIO output start failed.")
		_teardown()
		return
	set_process(true)
	print("[AudioRouter] ASIO output on '%s'." % driver)


# --- native (WASAPI / CoreAudio / ALSA) -------------------------------------

func _build_native_input(device: String) -> void:
	# Switch the device first, give the driver time to apply it, THEN start
	# capture. Starting capture immediately after set_input_device() races the
	# driver and fails with "init_input_device error".
	if device != "":
		AudioServer.set_input_device(device)
		await get_tree().create_timer(0.3).timeout
		if AudioServer.get_input_device() != device:
			AudioServer.set_input_device(device)
			await get_tree().create_timer(0.3).timeout

	_mic_player = AudioStreamPlayer.new()
	_mic_player.stream = AudioStreamMicrophone.new()
	_mic_player.bus = RECORD_BUS
	add_child(_mic_player)
	_mic_player.play()

func _build_native_output(device: String) -> void:
	if device != "":
		AudioServer.set_output_device(device)
	# Make sure the OS path is audible (an earlier ASIO session may have muted it).
	var m := AudioServer.get_bus_index("Master")
	if m >= 0:
		AudioServer.set_bus_mute(m, false)


# --- shared helpers ---------------------------------------------------------

func _setup_input_generator(rate: float, block: int) -> void:
	var gen := AudioStreamGenerator.new()
	gen.mix_rate = rate
	gen.buffer_length = maxf((block / rate) * 16.0, 0.1)
	_in_gen_player = AudioStreamPlayer.new()
	_in_gen_player.stream = gen
	_in_gen_player.bus = RECORD_BUS
	add_child(_in_gen_player)
	_in_gen_player.play()
	_in_playback = _in_gen_player.get_stream_playback() as AudioStreamGeneratorPlayback

func _on_asio_input_frame(data: PackedFloat32Array, channel: int) -> void:
	if channel == 0:
		_in_ch0 = data
		if not _in_stereo:
			_push_input(_in_ch0, _in_ch0)
	elif channel == 1:
		_in_ch1 = data
		if not _in_ch0.is_empty():
			_push_input(_in_ch0, _in_ch1)

func _push_input(left: PackedFloat32Array, right: PackedFloat32Array) -> void:
	if not _in_playback:
		return
	var n := left.size()
	if _in_playback.get_frames_available() < n:
		_in_ch0 = PackedFloat32Array(); _in_ch1 = PackedFloat32Array()
		return
	var frames := PackedVector2Array()
	frames.resize(n)
	for i in n:
		frames[i] = Vector2(left[i], right[i])
	_in_playback.push_buffer(frames)
	_in_ch0 = PackedFloat32Array(); _in_ch1 = PackedFloat32Array()


func _process(_delta: float) -> void:
	# Drive the ASIO output sink (full-duplex device or output-only) from Master.
	var sink = _device if _device != null else _asio_out
	if sink == null or _out_capture == null:
		return
	var avail := _out_capture.get_frames_available()
	if avail <= 0:
		return
	var room: int = sink.get_frames_free()
	var n: int = min(avail, room)
	if n > 0:
		sink.push_frames(_out_capture.get_buffer(n))


func _ensure_capture(bus_name: String) -> AudioEffectCapture:
	var idx := AudioServer.get_bus_index(bus_name)
	if idx == -1:
		return null
	for i in AudioServer.get_bus_effect_count(idx):
		if AudioServer.get_bus_effect(idx, i) is AudioEffectCapture:
			return AudioServer.get_bus_effect(idx, i)
	var cap := AudioEffectCapture.new()
	AudioServer.add_bus_effect(idx, cap)
	return cap


func _teardown() -> void:
	set_process(false)

	if _device:
		if _device.audio_frame.is_connected(_on_asio_input_frame):
			_device.audio_frame.disconnect(_on_asio_input_frame)
		_device.close(); _device.queue_free(); _device = null
	if _asio_in:
		if _asio_in.audio_frame.is_connected(_on_asio_input_frame):
			_asio_in.audio_frame.disconnect(_on_asio_input_frame)
		_asio_in.close(); _asio_in.queue_free(); _asio_in = null
	if _asio_out:
		_asio_out.close(); _asio_out.queue_free(); _asio_out = null

	if _mic_player:
		_mic_player.stop(); _mic_player.queue_free(); _mic_player = null
	if _in_gen_player:
		_in_gen_player.queue_free(); _in_gen_player = null
	_in_playback = null
	_out_capture = null
	_in_ch0 = PackedFloat32Array(); _in_ch1 = PackedFloat32Array()

	# Restore OS output and the Record bus (full-duplex mutes Record).
	var m := AudioServer.get_bus_index("Master")
	if m >= 0:
		AudioServer.set_bus_mute(m, false)
	var rec := AudioServer.get_bus_index(RECORD_BUS)
	if rec >= 0:
		AudioServer.set_bus_mute(rec, false)
