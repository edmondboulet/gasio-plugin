#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <vector>

// Forward declaration to keep ASIO headers out of Godot types
struct OutputDriverInfo;

namespace godot {

// Low-latency ASIO audio OUTPUT (Windows only).
//
// Usage from GDScript: capture Godot's audio (e.g. an AudioEffectCapture on the
// Master bus) and feed it here with push_frames(); the ASIO driver thread drains
// the ring buffer into the hardware. Mute Godot's own output to avoid hearing it
// twice through the OS device.
//
// Mirrors AsioAudioInput so the two can later be merged into a single
// full-duplex AsioAudioDevice (ASIO opens one driver for both directions).
class AsioAudioOutput : public Node {
    GDCLASS(AsioAudioOutput, Node)

public:
    AsioAudioOutput();
    ~AsioAudioOutput();

    // Installed ASIO driver names (same list as AsioAudioInput).
    TypedArray<String> get_driver_list() const;

    // Open the named driver and create OUTPUT buffers.
    // buffer_size: 0 = preferred, -1 = minimum (lowest latency), >0 = clamped.
    bool open(const String &driver_name, int buffer_size = 0);

    bool start();
    void stop();
    void close();

    bool is_running() const { return _running; }
    void show_control_panel();

    double get_sample_rate()     const { return _sample_rate; }
    int    get_buffer_size()     const { return _buffer_size; }
    int    get_min_buffer_size() const { return _min_buffer_size; }
    int    get_output_channels() const { return _output_channels; }

    // Queue stereo frames (L = x, R = y) for playback. Returns the number of
    // frames actually accepted (fewer than requested if the ring is full).
    int push_frames(const PackedVector2Array &frames);

    // Free space in the ring, in frames.
    int get_frames_free() const;

protected:
    static void _bind_methods();

private:
    static AsioAudioOutput *s_instance;

    static void  _cb_buffer_switch(long index, long direct_process);
    static void  _cb_sample_rate_changed(double rate);
    static long  _cb_asio_message(long selector, long value, void *msg, double *opt);
    static void *_cb_buffer_switch_time_info(void *time_info, long index, long direct_process);

    void _fill_output(long index);

    bool   _opened          = false; // this instance owns the open driver
    bool   _running         = false;
    double _sample_rate     = 0.0;
    int    _buffer_size     = 0;
    int    _min_buffer_size = 0;
    int    _output_channels = 0;

    OutputDriverInfo *_driver_info = nullptr;

    // Lock-free SPSC ring buffer of interleaved stereo float frames.
    // Producer: GDScript main thread (push_frames). Consumer: ASIO callback.
    std::vector<float>   _ring;        // size = _ring_frames * 2 (L,R)
    int                  _ring_frames = 0;
    std::atomic<int>     _write{0};    // frame index
    std::atomic<int>     _read{0};     // frame index
};

} // namespace godot
