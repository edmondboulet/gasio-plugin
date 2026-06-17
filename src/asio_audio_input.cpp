#include "asio_audio_input.hpp"

// ASIO SDK headers — must come before any Windows.h pollution from godot-cpp
#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "asiolist.h"

#include <cstring>
#include <cmath>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

// Defined in asiodrivers.cpp
extern AsioDrivers *asioDrivers;
bool loadAsioDriver(char *name);

// Maximum channels this host will handle
static constexpr int MAX_INPUT_CHANNELS  = 32;
static constexpr int MAX_OUTPUT_CHANNELS = 32;

// Internal struct that mirrors hostsample.cpp's DriverInfo
struct DriverInfo {
    ASIODriverInfo driverInfo;

    long inputChannels;
    long outputChannels;

    long minSize;
    long maxSize;
    long preferredSize;
    long granularity;

    ASIOSampleRate sampleRate;

    bool postOutput;

    long inputLatency;
    long outputLatency;

    long inputBuffers;
    long outputBuffers;
    ASIOBufferInfo  bufferInfos [MAX_INPUT_CHANNELS + MAX_OUTPUT_CHANNELS];
    ASIOChannelInfo channelInfos[MAX_INPUT_CHANNELS + MAX_OUTPUT_CHANNELS];
};

namespace godot {

// Static instance pointer — ASIO callbacks are plain C functions with no context.
AsioAudioInput *AsioAudioInput::s_instance = nullptr;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AsioAudioInput::AsioAudioInput() {
    _driver_info = new DriverInfo{};
}

AsioAudioInput::~AsioAudioInput() {
    close();
    delete _driver_info;
    _driver_info = nullptr;
}

// ---------------------------------------------------------------------------
// GDScript API
// ---------------------------------------------------------------------------

TypedArray<String> AsioAudioInput::get_driver_list() const {
    TypedArray<String> result;

    if (!asioDrivers)
        asioDrivers = new AsioDrivers();

    if (!asioDrivers)
        return result;

    char *names[64];
    char  buf[64][64];
    for (int i = 0; i < 64; ++i)
        names[i] = buf[i];

    long count = asioDrivers->getDriverNames(names, 64);
    for (long i = 0; i < count; ++i)
        result.append(String(names[i]));

    return result;
}

bool AsioAudioInput::open(const String &driver_name, int buffer_size) {
    close(); // ensure clean state

    s_instance = this;

    // loadAsioDriver expects a non-const char*
    CharString cs = driver_name.ascii();
    char name_buf[256];
    strncpy(name_buf, cs.get_data(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    if (!loadAsioDriver(name_buf)) {
        UtilityFunctions::printerr("AsioAudioInput: failed to load driver '", driver_name, "'");
        return false;
    }

    // On Windows, sysRef must be the application HWND.
    // Passing nullptr is acceptable for most modern ASIO drivers.
    _driver_info->driverInfo.sysRef = nullptr;

    if (ASIOInit(&_driver_info->driverInfo) != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioInput: ASIOInit() failed — ", _driver_info->driverInfo.errorMessage);
        asioDrivers->removeCurrentDriver();
        return false;
    }

    if (ASIOGetChannels(&_driver_info->inputChannels, &_driver_info->outputChannels) != ASE_OK) {
        ASIOExit();
        asioDrivers->removeCurrentDriver();
        return false;
    }

    if (ASIOGetBufferSize(&_driver_info->minSize, &_driver_info->maxSize,
                          &_driver_info->preferredSize, &_driver_info->granularity) != ASE_OK) {
        ASIOExit();
        asioDrivers->removeCurrentDriver();
        return false;
    }

    if (ASIOGetSampleRate(&_driver_info->sampleRate) != ASE_OK) {
        ASIOExit();
        asioDrivers->removeCurrentDriver();
        return false;
    }

    // Resolve requested buffer size:
    //   0  → driver preferred (default)
    //  -1  → driver minimum (lowest latency)
    //  >0  → clamp to [minSize, maxSize]
    long chosen;
    if (buffer_size == -1) {
        chosen = _driver_info->minSize;
    } else if (buffer_size <= 0) {
        chosen = _driver_info->preferredSize;
    } else {
        chosen = (long)std::max((int)_driver_info->minSize,
                     std::min((int)_driver_info->maxSize, buffer_size));
    }
    // Respect the driver's granularity for non-power-of-2 sizes.
    if (_driver_info->granularity > 0 && chosen != _driver_info->preferredSize) {
        long steps = (chosen - _driver_info->minSize) / _driver_info->granularity;
        chosen = _driver_info->minSize + steps * _driver_info->granularity;
    }

    // Cache public properties
    _sample_rate     = _driver_info->sampleRate;
    _buffer_size     = (int)chosen;
    _min_buffer_size = (int)_driver_info->minSize;
    _input_channels  = (int)std::min(_driver_info->inputChannels, (long)MAX_INPUT_CHANNELS);

    // Check for the ASIOOutputReady() optimisation
    _driver_info->postOutput = (ASIOOutputReady() == ASE_OK);

    // Build buffer info list — only request input channels, no outputs
    ASIOBufferInfo *info = _driver_info->bufferInfos;
    _driver_info->inputBuffers  = _input_channels;
    _driver_info->outputBuffers = 0;

    for (int i = 0; i < _input_channels; ++i, ++info) {
        info->isInput    = ASIOTrue;
        info->channelNum = i;
        info->buffers[0] = info->buffers[1] = nullptr;
    }

    // Set up callbacks
    static ASIOCallbacks callbacks;
    callbacks.bufferSwitch         = _cb_buffer_switch;
    callbacks.sampleRateDidChange  = _cb_sample_rate_changed;
    callbacks.asioMessage          = _cb_asio_message;
    callbacks.bufferSwitchTimeInfo = (ASIOTime *(*)(ASIOTime *, long, ASIOBool))_cb_buffer_switch_time_info;

    if (ASIOCreateBuffers(_driver_info->bufferInfos,
                          _driver_info->inputBuffers,
                          chosen,
                          &callbacks) != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioInput: ASIOCreateBuffers() failed");
        ASIOExit();
        asioDrivers->removeCurrentDriver();
        return false;
    }

    // Populate channel info
    for (int i = 0; i < _driver_info->inputBuffers; ++i) {
        _driver_info->channelInfos[i].channel = _driver_info->bufferInfos[i].channelNum;
        _driver_info->channelInfos[i].isInput  = ASIOTrue;
        ASIOGetChannelInfo(&_driver_info->channelInfos[i]);
    }

    ASIOGetLatencies(&_driver_info->inputLatency, &_driver_info->outputLatency);

    _opened = true;
    return true;
}

bool AsioAudioInput::start() {
    if (_running) return true;

    if (ASIOStart() != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioInput: ASIOStart() failed");
        return false;
    }

    _running = true;
    return true;
}

void AsioAudioInput::stop() {
    if (!_running) return;
    ASIOStop();
    _running = false;
}

void AsioAudioInput::close() {
    stop();

    // Only tear down the driver if THIS instance opened it. A stale instance
    // freed after another has opened a driver must not remove the live one.
    if (_opened) {
        ASIODisposeBuffers();
        ASIOExit();
        if (asioDrivers && asioDrivers->getCurrentDriverIndex() >= 0)
            asioDrivers->removeCurrentDriver();
        _opened = false;
    }

    if (_driver_info)
        *_driver_info = DriverInfo{};

    _sample_rate    = 0.0;
    _buffer_size    = 0;
    _input_channels = 0;

    if (s_instance == this)
        s_instance = nullptr;
}

void AsioAudioInput::show_control_panel() {
    ASIOControlPanel();
}

// ---------------------------------------------------------------------------
// Buffer processing — called from the ASIO callback thread
// ---------------------------------------------------------------------------

void AsioAudioInput::_process_buffers(long index) {
    // Use the size we actually created buffers with, not preferredSize.
    const int buf_samples = _buffer_size;

    for (int ch = 0; ch < _driver_info->inputBuffers; ++ch) {
        void *raw = _driver_info->bufferInfos[ch].buffers[index];
        if (!raw) continue;

        PackedFloat32Array audio;
        audio.resize(buf_samples);
        float *dst = audio.ptrw();

        ASIOSampleType sample_type = _driver_info->channelInfos[ch].type;

        switch (sample_type) {
            case ASIOSTInt16LSB: {
                const int16_t *src = static_cast<const int16_t *>(raw);
                for (int i = 0; i < buf_samples; ++i)
                    dst[i] = src[i] / 32768.0f;
                break;
            }
            case ASIOSTInt32LSB: {
                const int32_t *src = static_cast<const int32_t *>(raw);
                for (int i = 0; i < buf_samples; ++i)
                    dst[i] = src[i] / 2147483648.0f;
                break;
            }
            case ASIOSTFloat32LSB: {
                memcpy(dst, raw, (size_t)buf_samples * sizeof(float));
                break;
            }
            case ASIOSTFloat64LSB: {
                const double *src = static_cast<const double *>(raw);
                for (int i = 0; i < buf_samples; ++i)
                    dst[i] = (float)src[i];
                break;
            }
            case ASIOSTInt24LSB: {
                // 24-bit packed in 3 bytes, little-endian
                const uint8_t *src = static_cast<const uint8_t *>(raw);
                for (int i = 0; i < buf_samples; ++i) {
                    int32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
                    if (v & 0x800000) v |= 0xFF000000; // sign extend
                    dst[i] = v / 8388608.0f;
                    src += 3;
                }
                break;
            }
            default: {
                // Zero-fill for unsupported types
                memset(dst, 0, (size_t)buf_samples * sizeof(float));
                break;
            }
        }

        // Emit on the main thread via call_deferred so GDScript can use it safely.
        // For ultra-low-latency use, users can connect to the signal and process
        // audio directly; the deferred call trades one frame of latency for thread safety.
        call_deferred("emit_signal", "audio_frame", audio, ch);
    }

    if (_driver_info->postOutput)
        ASIOOutputReady();
}

// ---------------------------------------------------------------------------
// Static ASIO callbacks
// ---------------------------------------------------------------------------

void AsioAudioInput::_cb_buffer_switch(long index, long /*direct_process*/) {
    if (s_instance)
        s_instance->_process_buffers(index);
}

void *AsioAudioInput::_cb_buffer_switch_time_info(void * /*time_info*/, long index, long direct_process) {
    _cb_buffer_switch(index, direct_process);
    return nullptr;
}

void AsioAudioInput::_cb_sample_rate_changed(double rate) {
    if (s_instance) {
        s_instance->_sample_rate = rate;
        s_instance->call_deferred("emit_signal", "sample_rate_changed", rate);
    }
}

long AsioAudioInput::_cb_asio_message(long selector, long value, void * /*msg*/, double * /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            if (value == kAsioResetRequest
             || value == kAsioEngineVersion
             || value == kAsioResyncRequest
             || value == kAsioLatenciesChanged
             || value == kAsioSupportsTimeInfo
             || value == kAsioSupportsTimeCode
             || value == kAsioSupportsInputMonitor)
                return 1L;
            return 0L;
        case kAsioResetRequest:
            // Signal GDScript that a driver reset is needed
            if (s_instance)
                s_instance->call_deferred("emit_signal", "reset_requested");
            return 1L;
        case kAsioEngineVersion:
            return 2L;
        case kAsioSupportsTimeInfo:
            return 1L;
        case kAsioSupportsTimeCode:
            return 0L;
        default:
            return 0L;
    }
}

// ---------------------------------------------------------------------------
// Godot binding
// ---------------------------------------------------------------------------

void AsioAudioInput::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_driver_list"),                    &AsioAudioInput::get_driver_list);
    ClassDB::bind_method(D_METHOD("open", "driver_name", "buffer_size"), &AsioAudioInput::open, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("start"),                              &AsioAudioInput::start);
    ClassDB::bind_method(D_METHOD("stop"),                               &AsioAudioInput::stop);
    ClassDB::bind_method(D_METHOD("close"),                              &AsioAudioInput::close);
    ClassDB::bind_method(D_METHOD("is_running"),                         &AsioAudioInput::is_running);
    ClassDB::bind_method(D_METHOD("show_control_panel"),                 &AsioAudioInput::show_control_panel);
    ClassDB::bind_method(D_METHOD("get_sample_rate"),                    &AsioAudioInput::get_sample_rate);
    ClassDB::bind_method(D_METHOD("get_buffer_size"),                    &AsioAudioInput::get_buffer_size);
    ClassDB::bind_method(D_METHOD("get_min_buffer_size"),                &AsioAudioInput::get_min_buffer_size);
    ClassDB::bind_method(D_METHOD("get_input_channels"),                 &AsioAudioInput::get_input_channels);

    // Fired from the ASIO callback thread via call_deferred
    ADD_SIGNAL(MethodInfo("audio_frame",
        PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data"),
        PropertyInfo(Variant::INT, "channel")));

    ADD_SIGNAL(MethodInfo("sample_rate_changed",
        PropertyInfo(Variant::FLOAT, "new_rate")));

    ADD_SIGNAL(MethodInfo("reset_requested"));
}

} // namespace godot
