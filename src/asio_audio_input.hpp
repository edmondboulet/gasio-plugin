#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// Forward declarations to avoid pulling ASIO headers into Godot types
struct DriverInfo;

namespace godot {

class AsioAudioInput : public Node {
    GDCLASS(AsioAudioInput, Node)

public:
    AsioAudioInput();
    ~AsioAudioInput();

    // Returns a list of installed ASIO driver names.
    TypedArray<String> get_driver_list() const;

    // Opens and initialises the named ASIO driver.
    // buffer_size: samples per block. 0 = driver preferred (default),
    // -1 = driver minimum (lowest latency), >0 = clamped to [min, max].
    bool open(const String &driver_name, int buffer_size = 0);

    // Starts the audio stream. Call after open().
    bool start();

    // Stops the audio stream.
    void stop();

    // Closes the driver and frees ASIO resources.
    void close();

    bool is_running() const { return _running; }

    // Opens the driver's own control panel (if supported).
    void show_control_panel();

    double get_sample_rate()     const { return _sample_rate; }
    int    get_buffer_size()     const { return _buffer_size; }
    int    get_min_buffer_size() const { return _min_buffer_size; }
    int    get_input_channels()  const { return _input_channels; }

protected:
    static void _bind_methods();

private:
    // Static pointer so ASIO C-callbacks can reach the live instance.
    static AsioAudioInput *s_instance;

    // ASIO callback shims
    static void  _cb_buffer_switch(long index, long direct_process);
    static void  _cb_sample_rate_changed(double rate);
    static long  _cb_asio_message(long selector, long value, void *msg, double *opt);
    static void *_cb_buffer_switch_time_info(void *time_info, long index, long direct_process);

    void _process_buffers(long index);

    bool   _opened          = false; // this instance owns the open driver
    bool   _running         = false;
    double _sample_rate     = 0.0;
    int    _buffer_size     = 0;
    int    _min_buffer_size = 0;
    int    _input_channels  = 0;

    DriverInfo *_driver_info = nullptr;
};

} // namespace godot
