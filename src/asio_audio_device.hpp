#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <vector>

struct DeviceDriverInfo;

namespace godot {

// Full-duplex ASIO device (Windows only).
//
// Opens ONE ASIO driver with both input and output buffers — the correct way to
// use a single interface (e.g. a Focusrite) for capture and playback at once.
//
// - Direct monitoring: input is copied straight to the output on the ASIO
//   callback thread (set_direct_monitor), giving true low-latency monitoring
//   with no main-thread hop.
// - audio_frame: input blocks are also emitted (deferred to the main thread) for
//   pitch/chord detection or custom processing.
// - push_frames: game audio can be mixed into the output (same ring-buffer model
//   as AsioAudioOutput).
class AsioAudioDevice : public Node {
    GDCLASS(AsioAudioDevice, Node)

public:
    AsioAudioDevice();
    ~AsioAudioDevice();

    TypedArray<String> get_driver_list() const;

    // Open one driver with both input and output buffers.
    // buffer_size: 0 = preferred, -1 = minimum, >0 = clamped.
    bool open(const String &driver_name, int buffer_size = 0);

    bool start();
    void stop();
    void close();

    bool is_running() const { return _running; }
    void show_control_panel();

    double get_sample_rate()     const { return _sample_rate; }
    int    get_buffer_size()     const { return _buffer_size; }
    int    get_min_buffer_size() const { return _min_buffer_size; }
    int    get_input_channels()  const { return _input_channels; }
    int    get_output_channels() const { return _output_channels; }

    // Direct input->output monitoring on the ASIO thread.
    void  set_direct_monitor(bool enabled) { _monitor.store(enabled); }
    bool  is_direct_monitor() const { return _monitor.load(); }
    void  set_monitor_gain(float gain) { _monitor_gain.store(gain); }
    float get_monitor_gain() const { return _monitor_gain.load(); }

    // Mix stereo game audio (L=x, R=y) into the output. Returns frames accepted.
    int push_frames(const PackedVector2Array &frames);
    int get_frames_free() const;

protected:
    static void _bind_methods();

private:
    static AsioAudioDevice *s_instance;

    static void  _cb_buffer_switch(long index, long direct_process);
    static void  _cb_sample_rate_changed(double rate);
    static long  _cb_asio_message(long selector, long value, void *msg, double *opt);
    static void *_cb_buffer_switch_time_info(void *time_info, long index, long direct_process);

    void _process_block(long index);

    bool   _opened          = false; // this instance owns the open driver
    bool   _running         = false;
    double _sample_rate     = 0.0;
    int    _buffer_size     = 0;
    int    _min_buffer_size = 0;
    int    _input_channels  = 0;
    int    _output_channels = 0;

    std::atomic<bool>  _monitor{true};
    std::atomic<float> _monitor_gain{1.0f};

    DeviceDriverInfo *_driver_info = nullptr;

    // Per-input-channel float scratch for the current block (callback thread).
    std::vector<std::vector<float>> _in_float;

    // SPSC ring of interleaved stereo game audio (producer: main thread).
    std::vector<float> _ring;
    int                _ring_frames = 0;
    std::atomic<int>   _write{0};
    std::atomic<int>   _read{0};
};

} // namespace godot
