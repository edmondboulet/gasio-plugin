extends OptionButton

## Audio OUTPUT selector. Lists native (WASAPI/ALSA/CoreAudio) outputs AND ASIO
## drivers. Routing is handled by the AudioRouter autoload, which coordinates with
## the input selector — if both pick ASIO, it opens a single full-duplex device.

enum Kind { NATIVE, ASIO }


func _ready() -> void:
	await get_tree().create_timer(0.1).timeout
	_populate()
	item_selected.connect(_on_item_selected)


func _populate() -> void:
	clear()
	var id := 0

	var backend := _output_backend_label()
	add_separator("%s Outputs" % backend)
	for dev in AudioServer.get_output_device_list():
		add_item("%s: %s" % [backend, dev], id)
		set_item_metadata(get_item_index(id), {"kind": Kind.NATIVE, "name": dev})
		id += 1

	add_separator("ASIO Drivers")
	if not _asio_supported():
		add_item("ASIO: (unavailable on this platform)", id)
		set_item_disabled(get_item_index(id), true)
		return

	var probe = ClassDB.instantiate("AsioAudioOutput")
	add_child(probe)
	var drivers: Array = probe.get_driver_list()
	for d in drivers:
		add_item("ASIO: " + str(d), id)
		set_item_metadata(get_item_index(id), {"kind": Kind.ASIO, "name": str(d)})
		id += 1
	probe.queue_free()

	if drivers.is_empty():
		add_item("ASIO: (none installed)", id)
		set_item_disabled(get_item_index(id), true)


func _on_item_selected(index: int) -> void:
	var meta = get_item_metadata(index)
	if meta == null:
		return
	var kind := "asio" if meta["kind"] == Kind.ASIO else "native"
	AudioRouter.request_output(kind, meta["name"])


func _asio_supported() -> bool:
	return ClassDB.class_exists("AsioAudioOutput")


func _output_backend_label() -> String:
	match OS.get_name():
		"Windows": return "WASAPI"
		"macOS": return "CoreAudio"
		"Linux", "FreeBSD", "NetBSD", "OpenBSD", "BSD": return "ALSA/Pulse"
		_: return "Audio Out"
