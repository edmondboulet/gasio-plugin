#include "asio_audio_device.hpp"

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "asiolist.h"

#include <cstring>
#include <algorithm>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

extern AsioDrivers *asioDrivers;
bool loadAsioDriver(char *name);

static constexpr int MAX_INPUT_CHANNELS  = 32;
static constexpr int MAX_OUTPUT_CHANNELS = 32;

struct DeviceDriverInfo {
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
    // [0 .. inputBuffers)         = inputs
    // [inputBuffers .. +outputBuffers) = outputs
    ASIOBufferInfo  bufferInfos [MAX_INPUT_CHANNELS + MAX_OUTPUT_CHANNELS];
    ASIOChannelInfo channelInfos[MAX_INPUT_CHANNELS + MAX_OUTPUT_CHANNELS];
};

namespace godot {

AsioAudioDevice *AsioAudioDevice::s_instance = nullptr;

// ---- sample conversion helpers --------------------------------------------

static inline float sample_to_float(ASIOSampleType type, void *raw, int s) {
    switch (type) {
        case ASIOSTInt16LSB:   return ((const int16_t *)raw)[s] / 32768.0f;
        case ASIOSTInt32LSB:   return (float)(((const int32_t *)raw)[s] / 2147483648.0);
        case ASIOSTFloat32LSB: return ((const float *)raw)[s];
        case ASIOSTFloat64LSB: return (float)((const double *)raw)[s];
        case ASIOSTInt24LSB: {
            const uint8_t *p = (const uint8_t *)raw + (size_t)s * 3;
            int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= 0xFF000000;
            return v / 8388608.0f;
        }
        default: return 0.0f;
    }
}

static inline void float_to_sample(ASIOSampleType type, void *raw, int s, float v) {
    switch (type) {
        case ASIOSTInt16LSB: {
            double d = std::clamp((double)v * 32767.0, -32768.0, 32767.0);
            ((int16_t *)raw)[s] = (int16_t)d;
            break;
        }
        case ASIOSTInt32LSB: {
            double d = std::clamp((double)v * 2147483647.0, -2147483648.0, 2147483647.0);
            ((int32_t *)raw)[s] = (int32_t)d;
            break;
        }
        case ASIOSTFloat32LSB:
            ((float *)raw)[s] = v;
            break;
        case ASIOSTFloat64LSB:
            ((double *)raw)[s] = (double)v;
            break;
        case ASIOSTInt24LSB: {
            int32_t iv = (int32_t)std::clamp((double)v * 8388607.0, -8388608.0, 8388607.0);
            uint8_t *p = (uint8_t *)raw + (size_t)s * 3;
            p[0] = iv & 0xFF; p[1] = (iv >> 8) & 0xFF; p[2] = (iv >> 16) & 0xFF;
            break;
        }
        default: break;
    }
}

// ---------------------------------------------------------------------------

AsioAudioDevice::AsioAudioDevice() {
    _driver_info = new DeviceDriverInfo{};
}

AsioAudioDevice::~AsioAudioDevice() {
    close();
    delete _driver_info;
    _driver_info = nullptr;
}

TypedArray<String> AsioAudioDevice::get_driver_list() const {
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

bool AsioAudioDevice::open(const String &driver_name, int buffer_size) {
    close();
    s_instance = this;

    CharString cs = driver_name.ascii();
    char name_buf[256];
    strncpy(name_buf, cs.get_data(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    if (!loadAsioDriver(name_buf)) {
        UtilityFunctions::printerr("AsioAudioDevice: failed to load driver '", driver_name, "'");
        return false;
    }

    _driver_info->driverInfo.sysRef = nullptr;
    if (ASIOInit(&_driver_info->driverInfo) != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioDevice: ASIOInit() failed — ", _driver_info->driverInfo.errorMessage);
        asioDrivers->removeCurrentDriver();
        return false;
    }
    if (ASIOGetChannels(&_driver_info->inputChannels, &_driver_info->outputChannels) != ASE_OK) {
        ASIOExit(); asioDrivers->removeCurrentDriver(); return false;
    }
    if (ASIOGetBufferSize(&_driver_info->minSize, &_driver_info->maxSize,
                          &_driver_info->preferredSize, &_driver_info->granularity) != ASE_OK) {
        ASIOExit(); asioDrivers->removeCurrentDriver(); return false;
    }
    if (ASIOGetSampleRate(&_driver_info->sampleRate) != ASE_OK) {
        ASIOExit(); asioDrivers->removeCurrentDriver(); return false;
    }

    long chosen;
    if (buffer_size == -1) chosen = _driver_info->minSize;
    else if (buffer_size <= 0) chosen = _driver_info->preferredSize;
    else chosen = (long)std::max((int)_driver_info->minSize, std::min((int)_driver_info->maxSize, buffer_size));
    if (_driver_info->granularity > 0 && chosen != _driver_info->preferredSize) {
        long steps = (chosen - _driver_info->minSize) / _driver_info->granularity;
        chosen = _driver_info->minSize + steps * _driver_info->granularity;
    }

    _sample_rate     = _driver_info->sampleRate;
    _buffer_size     = (int)chosen;
    _min_buffer_size = (int)_driver_info->minSize;
    _input_channels  = (int)std::min(_driver_info->inputChannels, (long)MAX_INPUT_CHANNELS);
    _output_channels = (int)std::min(_driver_info->outputChannels, (long)MAX_OUTPUT_CHANNELS);
    _driver_info->postOutput = (ASIOOutputReady() == ASE_OK);

    // Build buffer list: inputs first, then outputs.
    ASIOBufferInfo *info = _driver_info->bufferInfos;
    _driver_info->inputBuffers  = _input_channels;
    _driver_info->outputBuffers = _output_channels;
    for (int i = 0; i < _input_channels; ++i, ++info) {
        info->isInput = ASIOTrue;  info->channelNum = i;
        info->buffers[0] = info->buffers[1] = nullptr;
    }
    for (int i = 0; i < _output_channels; ++i, ++info) {
        info->isInput = ASIOFalse; info->channelNum = i;
        info->buffers[0] = info->buffers[1] = nullptr;
    }

    static ASIOCallbacks callbacks;
    callbacks.bufferSwitch         = _cb_buffer_switch;
    callbacks.sampleRateDidChange  = _cb_sample_rate_changed;
    callbacks.asioMessage          = _cb_asio_message;
    callbacks.bufferSwitchTimeInfo = (ASIOTime *(*)(ASIOTime *, long, ASIOBool))_cb_buffer_switch_time_info;

    long total = _driver_info->inputBuffers + _driver_info->outputBuffers;
    if (ASIOCreateBuffers(_driver_info->bufferInfos, total, chosen, &callbacks) != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioDevice: ASIOCreateBuffers() failed");
        ASIOExit(); asioDrivers->removeCurrentDriver(); return false;
    }

    for (int i = 0; i < total; ++i) {
        _driver_info->channelInfos[i].channel = _driver_info->bufferInfos[i].channelNum;
        _driver_info->channelInfos[i].isInput = _driver_info->bufferInfos[i].isInput;
        ASIOGetChannelInfo(&_driver_info->channelInfos[i]);
    }

    ASIOGetLatencies(&_driver_info->inputLatency, &_driver_info->outputLatency);

    // Float scratch for input conversion.
    _in_float.assign(_input_channels, std::vector<float>(_buffer_size, 0.0f));

    // Ring for game audio (time-based; see AsioAudioOutput for rationale).
    int want = std::max(_buffer_size * 8, (int)(_sample_rate * 0.1));
    int cap = 1;
    while (cap < want) cap <<= 1;
    _ring_frames = cap;
    _ring.assign((size_t)_ring_frames * 2, 0.0f);
    _write.store(0);
    _read.store(0);

    _opened = true;
    return true;
}

bool AsioAudioDevice::start() {
    if (_running) return true;
    if (ASIOStart() != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioDevice: ASIOStart() failed");
        return false;
    }
    _running = true;
    return true;
}

void AsioAudioDevice::stop() {
    if (!_running) return;
    ASIOStop();
    _running = false;
}

void AsioAudioDevice::close() {
    stop();
    if (_opened) {
        ASIODisposeBuffers();
        ASIOExit();
        if (asioDrivers && asioDrivers->getCurrentDriverIndex() >= 0)
            asioDrivers->removeCurrentDriver();
        _opened = false;
    }
    if (_driver_info)
        *_driver_info = DeviceDriverInfo{};

    _sample_rate = 0.0; _buffer_size = 0;
    _input_channels = 0; _output_channels = 0;
    _in_float.clear();
    _ring.clear(); _ring_frames = 0;
    _write.store(0); _read.store(0);

    if (s_instance == this)
        s_instance = nullptr;
}

void AsioAudioDevice::show_control_panel() {
    ASIOControlPanel();
}

// ---- ring (producer: main thread) -----------------------------------------

int AsioAudioDevice::get_frames_free() const {
    if (_ring_frames == 0) return 0;
    int w = _write.load(std::memory_order_relaxed);
    int r = _read.load(std::memory_order_acquire);
    int used = (w - r + _ring_frames) % _ring_frames;
    return _ring_frames - 1 - used;
}

int AsioAudioDevice::push_frames(const PackedVector2Array &frames) {
    if (_ring_frames == 0) return 0;
    int free = get_frames_free();
    int n = (int)std::min((int64_t)frames.size(), (int64_t)free);
    int w = _write.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        const Vector2 &f = frames[i];
        _ring[(size_t)w * 2 + 0] = f.x;
        _ring[(size_t)w * 2 + 1] = f.y;
        w = (w + 1) % _ring_frames;
    }
    _write.store(w, std::memory_order_release);
    return n;
}

// ---- callback (consumer/producer thread) ----------------------------------

void AsioAudioDevice::_process_block(long index) {
    const int frames = _buffer_size;
    const int in_n   = _driver_info->inputBuffers;
    const int out_n  = _driver_info->outputBuffers;
    const int out_base = in_n; // outputs start here in bufferInfos

    // 1. Convert all input channels to float.
    for (int ic = 0; ic < in_n; ++ic) {
        void *raw = _driver_info->bufferInfos[ic].buffers[index];
        ASIOSampleType type = _driver_info->channelInfos[ic].type;
        float *dst = _in_float[ic].data();
        if (raw) {
            for (int s = 0; s < frames; ++s)
                dst[s] = sample_to_float(type, raw, s);
        } else {
            std::fill(dst, dst + frames, 0.0f);
        }
    }

    const bool monitor = _monitor.load(std::memory_order_relaxed);
    const float gain   = _monitor_gain.load(std::memory_order_relaxed);

    int r = _read.load(std::memory_order_relaxed);
    int w = _write.load(std::memory_order_acquire);

    // 2. Build and write output: direct monitor (in->out) + game audio (ring).
    for (int s = 0; s < frames; ++s) {
        float gL = 0.0f, gR = 0.0f;
        if (r != w) {
            gL = _ring[(size_t)r * 2 + 0];
            gR = _ring[(size_t)r * 2 + 1];
            r = (r + 1) % _ring_frames;
        }

        float mL = 0.0f, mR = 0.0f;
        if (monitor && in_n > 0) {
            mL = _in_float[0][s] * gain;
            mR = _in_float[in_n > 1 ? 1 : 0][s] * gain;
        }

        float oL = mL + gL;
        float oR = mR + gR;

        for (int oc = 0; oc < out_n; ++oc) {
            void *raw = _driver_info->bufferInfos[out_base + oc].buffers[index];
            if (!raw) continue;
            float v = (oc == 0) ? oL : (oc == 1 ? oR : 0.0f);
            float_to_sample(_driver_info->channelInfos[out_base + oc].type, raw, s, v);
        }
    }
    _read.store(r, std::memory_order_release);

    // 3. Emit input blocks for detection / custom processing (main thread).
    for (int ic = 0; ic < in_n; ++ic) {
        PackedFloat32Array arr;
        arr.resize(frames);
        memcpy(arr.ptrw(), _in_float[ic].data(), (size_t)frames * sizeof(float));
        call_deferred("emit_signal", "audio_frame", arr, ic);
    }

    if (_driver_info->postOutput)
        ASIOOutputReady();
}

// ---- static callbacks ------------------------------------------------------

void AsioAudioDevice::_cb_buffer_switch(long index, long /*direct*/) {
    if (s_instance) s_instance->_process_block(index);
}

void *AsioAudioDevice::_cb_buffer_switch_time_info(void * /*ti*/, long index, long direct) {
    _cb_buffer_switch(index, direct);
    return nullptr;
}

void AsioAudioDevice::_cb_sample_rate_changed(double rate) {
    if (s_instance) {
        s_instance->_sample_rate = rate;
        s_instance->call_deferred("emit_signal", "sample_rate_changed", rate);
    }
}

long AsioAudioDevice::_cb_asio_message(long selector, long value, void * /*msg*/, double * /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            if (value == kAsioResetRequest || value == kAsioEngineVersion
             || value == kAsioResyncRequest || value == kAsioLatenciesChanged
             || value == kAsioSupportsTimeInfo)
                return 1L;
            return 0L;
        case kAsioResetRequest:
            if (s_instance) s_instance->call_deferred("emit_signal", "reset_requested");
            return 1L;
        case kAsioEngineVersion: return 2L;
        case kAsioSupportsTimeInfo: return 1L;
        default: return 0L;
    }
}

// ---------------------------------------------------------------------------

void AsioAudioDevice::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_driver_list"),                    &AsioAudioDevice::get_driver_list);
    ClassDB::bind_method(D_METHOD("open", "driver_name", "buffer_size"), &AsioAudioDevice::open, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("start"),                              &AsioAudioDevice::start);
    ClassDB::bind_method(D_METHOD("stop"),                               &AsioAudioDevice::stop);
    ClassDB::bind_method(D_METHOD("close"),                              &AsioAudioDevice::close);
    ClassDB::bind_method(D_METHOD("is_running"),                         &AsioAudioDevice::is_running);
    ClassDB::bind_method(D_METHOD("show_control_panel"),                 &AsioAudioDevice::show_control_panel);
    ClassDB::bind_method(D_METHOD("get_sample_rate"),                    &AsioAudioDevice::get_sample_rate);
    ClassDB::bind_method(D_METHOD("get_buffer_size"),                    &AsioAudioDevice::get_buffer_size);
    ClassDB::bind_method(D_METHOD("get_min_buffer_size"),                &AsioAudioDevice::get_min_buffer_size);
    ClassDB::bind_method(D_METHOD("get_input_channels"),                 &AsioAudioDevice::get_input_channels);
    ClassDB::bind_method(D_METHOD("get_output_channels"),                &AsioAudioDevice::get_output_channels);
    ClassDB::bind_method(D_METHOD("set_direct_monitor", "enabled"),      &AsioAudioDevice::set_direct_monitor);
    ClassDB::bind_method(D_METHOD("is_direct_monitor"),                  &AsioAudioDevice::is_direct_monitor);
    ClassDB::bind_method(D_METHOD("set_monitor_gain", "gain"),           &AsioAudioDevice::set_monitor_gain);
    ClassDB::bind_method(D_METHOD("get_monitor_gain"),                   &AsioAudioDevice::get_monitor_gain);
    ClassDB::bind_method(D_METHOD("push_frames", "frames"),             &AsioAudioDevice::push_frames);
    ClassDB::bind_method(D_METHOD("get_frames_free"),                    &AsioAudioDevice::get_frames_free);

    ADD_SIGNAL(MethodInfo("audio_frame",
        PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data"),
        PropertyInfo(Variant::INT, "channel")));
    ADD_SIGNAL(MethodInfo("sample_rate_changed", PropertyInfo(Variant::FLOAT, "new_rate")));
    ADD_SIGNAL(MethodInfo("reset_requested"));
}

} // namespace godot
