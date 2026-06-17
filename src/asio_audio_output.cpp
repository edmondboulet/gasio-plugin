#include "asio_audio_output.hpp"

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include "asiolist.h"

#include <cstring>
#include <algorithm>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// Defined in asiodrivers.cpp (shared with AsioAudioInput)
extern AsioDrivers *asioDrivers;
bool loadAsioDriver(char *name);

static constexpr int MAX_OUTPUT_CHANNELS = 32;

struct OutputDriverInfo {
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
    long outputBuffers;
    ASIOBufferInfo  bufferInfos [MAX_OUTPUT_CHANNELS];
    ASIOChannelInfo channelInfos[MAX_OUTPUT_CHANNELS];
};

namespace godot {

AsioAudioOutput *AsioAudioOutput::s_instance = nullptr;

// ---------------------------------------------------------------------------

AsioAudioOutput::AsioAudioOutput() {
    _driver_info = new OutputDriverInfo{};
}

AsioAudioOutput::~AsioAudioOutput() {
    close();
    delete _driver_info;
    _driver_info = nullptr;
}

// ---------------------------------------------------------------------------

TypedArray<String> AsioAudioOutput::get_driver_list() const {
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

bool AsioAudioOutput::open(const String &driver_name, int buffer_size) {
    close();
    s_instance = this;

    CharString cs = driver_name.ascii();
    char name_buf[256];
    strncpy(name_buf, cs.get_data(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';

    if (!loadAsioDriver(name_buf)) {
        UtilityFunctions::printerr("AsioAudioOutput: failed to load driver '", driver_name, "'");
        return false;
    }

    _driver_info->driverInfo.sysRef = nullptr;
    if (ASIOInit(&_driver_info->driverInfo) != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioOutput: ASIOInit() failed — ", _driver_info->driverInfo.errorMessage);
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
    if (buffer_size == -1) {
        chosen = _driver_info->minSize;
    } else if (buffer_size <= 0) {
        chosen = _driver_info->preferredSize;
    } else {
        chosen = (long)std::max((int)_driver_info->minSize,
                     std::min((int)_driver_info->maxSize, buffer_size));
    }
    if (_driver_info->granularity > 0 && chosen != _driver_info->preferredSize) {
        long steps = (chosen - _driver_info->minSize) / _driver_info->granularity;
        chosen = _driver_info->minSize + steps * _driver_info->granularity;
    }

    _sample_rate     = _driver_info->sampleRate;
    _buffer_size     = (int)chosen;
    _min_buffer_size = (int)_driver_info->minSize;
    _output_channels = (int)std::min(_driver_info->outputChannels, (long)MAX_OUTPUT_CHANNELS);

    _driver_info->postOutput = (ASIOOutputReady() == ASE_OK);

    // Request output channels only (no inputs).
    ASIOBufferInfo *info = _driver_info->bufferInfos;
    _driver_info->outputBuffers = _output_channels;
    for (int i = 0; i < _output_channels; ++i, ++info) {
        info->isInput    = ASIOFalse;
        info->channelNum = i;
        info->buffers[0] = info->buffers[1] = nullptr;
    }

    static ASIOCallbacks callbacks;
    callbacks.bufferSwitch         = _cb_buffer_switch;
    callbacks.sampleRateDidChange  = _cb_sample_rate_changed;
    callbacks.asioMessage          = _cb_asio_message;
    callbacks.bufferSwitchTimeInfo = (ASIOTime *(*)(ASIOTime *, long, ASIOBool))_cb_buffer_switch_time_info;

    if (ASIOCreateBuffers(_driver_info->bufferInfos, _driver_info->outputBuffers,
                          chosen, &callbacks) != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioOutput: ASIOCreateBuffers() failed");
        ASIOExit(); asioDrivers->removeCurrentDriver(); return false;
    }

    for (int i = 0; i < _driver_info->outputBuffers; ++i) {
        _driver_info->channelInfos[i].channel  = _driver_info->bufferInfos[i].channelNum;
        _driver_info->channelInfos[i].isInput  = ASIOFalse;
        ASIOGetChannelInfo(&_driver_info->channelInfos[i]);
    }

    ASIOGetLatencies(&_driver_info->inputLatency, &_driver_info->outputLatency);

    // Size the ring by TIME, not by ASIO block size. We feed it from Godot's
    // main thread (~60fps, ~16ms bursts), while the ASIO callback drains it in
    // tiny blocks (16 samples = 0.33ms). The ring must bridge the main-thread
    // cadence, so use at least ~100ms regardless of how small the ASIO buffer is.
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

bool AsioAudioOutput::start() {
    if (_running) return true;
    if (ASIOStart() != ASE_OK) {
        UtilityFunctions::printerr("AsioAudioOutput: ASIOStart() failed");
        return false;
    }
    _running = true;
    return true;
}

void AsioAudioOutput::stop() {
    if (!_running) return;
    ASIOStop();
    _running = false;
}

void AsioAudioOutput::close() {
    stop();
    if (_opened) {
        ASIODisposeBuffers();
        ASIOExit();
        if (asioDrivers && asioDrivers->getCurrentDriverIndex() >= 0)
            asioDrivers->removeCurrentDriver();
        _opened = false;
    }
    if (_driver_info)
        *_driver_info = OutputDriverInfo{};

    _sample_rate = 0.0;
    _buffer_size = 0;
    _output_channels = 0;
    _ring_frames = 0;
    _ring.clear();
    _write.store(0);
    _read.store(0);

    if (s_instance == this)
        s_instance = nullptr;
}

void AsioAudioOutput::show_control_panel() {
    ASIOControlPanel();
}

// ---------------------------------------------------------------------------
// Ring buffer (producer: GDScript main thread)
// ---------------------------------------------------------------------------

int AsioAudioOutput::get_frames_free() const {
    if (_ring_frames == 0) return 0;
    int w = _write.load(std::memory_order_relaxed);
    int r = _read.load(std::memory_order_acquire);
    int used = (w - r + _ring_frames) % _ring_frames;
    return _ring_frames - 1 - used; // keep one slot empty to distinguish full/empty
}

int AsioAudioOutput::push_frames(const PackedVector2Array &frames) {
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

// ---------------------------------------------------------------------------
// Output fill (consumer: ASIO callback thread)
// ---------------------------------------------------------------------------

void AsioAudioOutput::_fill_output(long index) {
    const int frames = _buffer_size;
    const int chans = _driver_info->outputBuffers;

    int r = _read.load(std::memory_order_relaxed);
    int w = _write.load(std::memory_order_acquire);

    for (int s = 0; s < frames; ++s) {
        float left = 0.0f, right = 0.0f;
        if (r != w) { // data available
            left  = _ring[(size_t)r * 2 + 0];
            right = _ring[(size_t)r * 2 + 1];
            r = (r + 1) % _ring_frames;
        } // else underrun -> silence

        for (int ch = 0; ch < chans; ++ch) {
            void *raw = _driver_info->bufferInfos[ch].buffers[index];
            if (!raw) continue;
            float v = (ch == 0) ? left : (ch == 1 ? right : 0.0f);

            switch (_driver_info->channelInfos[ch].type) {
                case ASIOSTInt16LSB: {
                    double d = (double)v * 32767.0;
                    d = std::clamp(d, -32768.0, 32767.0);
                    ((int16_t *)raw)[s] = (int16_t)d;
                    break;
                }
                case ASIOSTInt32LSB: {
                    // Scale in double: 2147483647.0f as a float rounds to 2^31,
                    // which overflows int32 on cast and wraps loud samples to the
                    // opposite sign (audible as heavy saturation). double is exact.
                    double d = (double)v * 2147483647.0;
                    d = std::clamp(d, -2147483648.0, 2147483647.0);
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
                    int32_t iv = (int32_t)std::clamp(v * 8388607.0f, -8388608.0f, 8388607.0f);
                    uint8_t *dst = (uint8_t *)raw + (size_t)s * 3;
                    dst[0] = iv & 0xFF;
                    dst[1] = (iv >> 8) & 0xFF;
                    dst[2] = (iv >> 16) & 0xFF;
                    break;
                }
                default:
                    break;
            }
        }
    }

    _read.store(r, std::memory_order_release);

    if (_driver_info->postOutput)
        ASIOOutputReady();
}

// ---------------------------------------------------------------------------
// Static ASIO callbacks
// ---------------------------------------------------------------------------

void AsioAudioOutput::_cb_buffer_switch(long index, long /*direct_process*/) {
    if (s_instance)
        s_instance->_fill_output(index);
}

void *AsioAudioOutput::_cb_buffer_switch_time_info(void * /*time_info*/, long index, long direct_process) {
    _cb_buffer_switch(index, direct_process);
    return nullptr;
}

void AsioAudioOutput::_cb_sample_rate_changed(double rate) {
    if (s_instance) {
        s_instance->_sample_rate = rate;
        s_instance->call_deferred("emit_signal", "sample_rate_changed", rate);
    }
}

long AsioAudioOutput::_cb_asio_message(long selector, long value, void * /*msg*/, double * /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            if (value == kAsioResetRequest || value == kAsioEngineVersion
             || value == kAsioResyncRequest || value == kAsioLatenciesChanged
             || value == kAsioSupportsTimeInfo)
                return 1L;
            return 0L;
        case kAsioResetRequest:
            if (s_instance)
                s_instance->call_deferred("emit_signal", "reset_requested");
            return 1L;
        case kAsioEngineVersion:
            return 2L;
        case kAsioSupportsTimeInfo:
            return 1L;
        default:
            return 0L;
    }
}

// ---------------------------------------------------------------------------

void AsioAudioOutput::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_driver_list"),                    &AsioAudioOutput::get_driver_list);
    ClassDB::bind_method(D_METHOD("open", "driver_name", "buffer_size"), &AsioAudioOutput::open, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("start"),                              &AsioAudioOutput::start);
    ClassDB::bind_method(D_METHOD("stop"),                               &AsioAudioOutput::stop);
    ClassDB::bind_method(D_METHOD("close"),                              &AsioAudioOutput::close);
    ClassDB::bind_method(D_METHOD("is_running"),                         &AsioAudioOutput::is_running);
    ClassDB::bind_method(D_METHOD("show_control_panel"),                 &AsioAudioOutput::show_control_panel);
    ClassDB::bind_method(D_METHOD("get_sample_rate"),                    &AsioAudioOutput::get_sample_rate);
    ClassDB::bind_method(D_METHOD("get_buffer_size"),                    &AsioAudioOutput::get_buffer_size);
    ClassDB::bind_method(D_METHOD("get_min_buffer_size"),                &AsioAudioOutput::get_min_buffer_size);
    ClassDB::bind_method(D_METHOD("get_output_channels"),                &AsioAudioOutput::get_output_channels);
    ClassDB::bind_method(D_METHOD("push_frames", "frames"),              &AsioAudioOutput::push_frames);
    ClassDB::bind_method(D_METHOD("get_frames_free"),                    &AsioAudioOutput::get_frames_free);

    ADD_SIGNAL(MethodInfo("sample_rate_changed",
        PropertyInfo(Variant::FLOAT, "new_rate")));
    ADD_SIGNAL(MethodInfo("reset_requested"));
}

} // namespace godot
