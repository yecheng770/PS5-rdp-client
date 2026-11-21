/* Windows rdpsnd -> bounded PCM ring -> PS5 main (HDMI/TV) output port.
 * No network callback waits for the audio device. Only the output thread calls
 * sceAudioOutOutput; Close joins it before closing its port or freeing memory.
 * The public FreeRDP device interface is intentionally kept in this file. */
#include "psrdp_audio.h"
#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/client/rdpsnd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int user, int type, int index, unsigned frames,
                          unsigned rate, unsigned format);
extern int sceAudioOutOutput(int handle, const void *samples);
extern int sceAudioOutClose(int handle);
extern void psrdp_diag_line(const char *line);
extern BOOL VCAPITYPE rdpsnd_VirtualChannelEntryEx(PCHANNEL_ENTRY_POINTS_EX, PVOID);

#define AUDIO_RATE 48000u
#define AUDIO_GRAIN 256u
/* Headroom for network bursts. Playback starts with the first available block. */
#define AUDIO_CAPACITY (AUDIO_RATE * 2u)
#define AUDIO_IDLE_FRAMES (AUDIO_RATE * 80u / 1000u)

typedef struct {
    rdpsndDevicePlugin device; /* must be first */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    int thread_started, running, port, failed, started;
    AUDIO_FORMAT format;
    uint32_t volume;
    int16_t ring[AUDIO_CAPACITY][2];
    size_t read, count;
    int output_logged, discard_logged;
    uint64_t received, submitted, dropped, underruns, malformed;
    size_t max_packet_frames, peak_pending_frames;
    uint64_t overflow_packets;
    size_t quiet_frames, empty_frames, onset_offset;
    uint64_t onset_ms;
    int onset_pending;
    unsigned onset_logs;
    int16_t previous[2];
    unsigned phase;
    int have_previous;
} psrdpAudio;

static void audio_log(const char *what, int value)
{
    char line[160];
    snprintf(line, sizeof(line), "audio: %s=%d", what, value);
    psrdp_diag_line(line);
}

static uint64_t audio_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (unsigned long)ts.tv_nsec / 1000000u;
}

static BOOL audio_format_supported(rdpsndDevicePlugin *device, const AUDIO_FORMAT *f)
{
    (void)device;
    if (!f || f->wFormatTag != WAVE_FORMAT_PCM || f->cbSize != 0 ||
        (f->nChannels != 1 && f->nChannels != 2) ||
        f->nSamplesPerSec < 8000 || f->nSamplesPerSec > 192000 ||
        (f->wBitsPerSample != 8 && f->wBitsPerSample != 16 &&
         f->wBitsPerSample != 24 && f->wBitsPerSample != 32)) return FALSE;
    return f->nBlockAlign == f->nChannels * (f->wBitsPerSample / 8u);
}

/* FreeRDP's built-in MS/IMA ADPCM decoders emit 16-bit PCM. The wire
 * nBlockAlign describes a compressed block, not the decoded frame. */
static BOOL audio_default_format(rdpsndDevicePlugin *device, const AUDIO_FORMAT *wire,
                                 AUDIO_FORMAT *pcm)
{
    (void)device;
    if (!wire || !pcm || (wire->wFormatTag != WAVE_FORMAT_ADPCM &&
                          wire->wFormatTag != WAVE_FORMAT_DVI_ADPCM) ||
        (wire->nChannels != 1 && wire->nChannels != 2) ||
        wire->nSamplesPerSec < 8000 || wire->nSamplesPerSec > 192000 ||
        wire->nBlockAlign < (wire->wFormatTag == WAVE_FORMAT_ADPCM ? 7u : 4u) * wire->nChannels)
        return FALSE;
    *pcm = (AUDIO_FORMAT) {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = wire->nChannels,
        .nSamplesPerSec = wire->nSamplesPerSec,
        .nAvgBytesPerSec = wire->nSamplesPerSec * wire->nChannels * 2u,
        .nBlockAlign = wire->nChannels * 2u,
        .wBitsPerSample = 16,
    };
    return TRUE;
}

static void *audio_output(void *arg)
{
    psrdpAudio *a = arg;
    /* PS5 consumes exactly AUDIO_GRAIN interleaved frames per call. */
    _Alignas(64) int16_t block[AUDIO_GRAIN][2];
    for (;;) {
        size_t n;
        pthread_mutex_lock(&a->lock);
        while (a->running && !a->started && !a->count)
            pthread_cond_wait(&a->wake, &a->lock);
        if (!a->running) { pthread_mutex_unlock(&a->lock); break; }
        a->started = 1;
        n = a->count < AUDIO_GRAIN ? a->count : AUDIO_GRAIN;
        memset(block, 0, sizeof(block));
        for (size_t i = 0; i < n; i++) {
            block[i][0] = (int16_t)((int64_t)a->ring[a->read][0] * (a->volume & 0xffffu) / 65535);
            block[i][1] = (int16_t)((int64_t)a->ring[a->read][1] * (a->volume >> 16) / 65535);
            a->read = (a->read + 1) % AUDIO_CAPACITY;
        }
        uint64_t onset_ms = 0;
        size_t onset_offset = 0;
        if (a->onset_pending && a->onset_offset < n) {
            onset_ms = a->onset_ms;
            onset_offset = a->onset_offset;
            a->onset_pending = 0;
            a->onset_logs++;
        } else if (a->onset_pending) {
            a->onset_offset -= n;
        }
        a->count -= n;
        if (n < AUDIO_GRAIN) {
            a->underruns++;
            a->empty_frames += AUDIO_GRAIN - n;
            if (a->empty_frames >= AUDIO_IDLE_FRAMES) {
                a->empty_frames = AUDIO_IDLE_FRAMES;
                a->quiet_frames = AUDIO_IDLE_FRAMES;
            }
        }
        if (n == AUDIO_GRAIN) a->empty_frames = 0;
        pthread_mutex_unlock(&a->lock);
        uint64_t before_output = onset_ms ? audio_now_ms() : 0;
        /* The main port paces at 48 kHz, including silence after starvation. */
        int rc = sceAudioOutOutput(a->port, block);
        uint64_t after_output = onset_ms ? audio_now_ms() : 0;
        pthread_mutex_lock(&a->lock);
        if (rc < 0) { a->failed = 1; a->running = 0; }
        else a->submitted += n;
        int first_signal = 0;
        if (rc >= 0 && !a->output_logged) {
            for (size_t i = 0; i < n; ++i) {
                if (block[i][0] || block[i][1]) { first_signal = a->output_logged = 1; break; }
            }
        }
        pthread_mutex_unlock(&a->lock);
        if (rc < 0) { audio_log("output failed", rc); break; }
        if (onset_ms && before_output >= onset_ms && after_output >= before_output) {
            char line[160];
            snprintf(line, sizeof(line),
                     "audio: idle onset packet_to_submit_ms=%llu output_call_ms=%llu grain_offset_ms=%zu",
                     (unsigned long long)(before_output - onset_ms),
                     (unsigned long long)(after_output - before_output),
                     onset_offset * 1000u / AUDIO_RATE);
            psrdp_diag_line(line);
        }
        if (first_signal) audio_log("first nonzero PCM block submitted to main output; handle", a->port);
    }
    return NULL;
}

static void audio_close(rdpsndDevicePlugin *device)
{
    psrdpAudio *a = (psrdpAudio *)device;
    pthread_mutex_lock(&a->lock);
    a->running = 0;
    pthread_cond_signal(&a->wake);
    pthread_mutex_unlock(&a->lock);
    if (a->thread_started) {
        pthread_join(a->thread, NULL);
        a->thread_started = 0;
    }
    if (a->port >= 0) {
        int rc = sceAudioOutClose(a->port);
        if (rc < 0) audio_log("close failed", rc);
        a->port = -1;
        char line[256];
        snprintf(line, sizeof(line),
                 "audio: closed input_frames=%llu played_frames=%llu dropped_frames=%llu underrun_blocks=%llu malformed=%llu",
                 (unsigned long long)a->received, (unsigned long long)a->submitted,
                 (unsigned long long)a->dropped, (unsigned long long)a->underruns,
                 (unsigned long long)a->malformed);
        psrdp_diag_line(line);
        snprintf(line, sizeof(line),
                 "audio: peak_queue_ms=%zu largest_packet_ms=%zu overflow_packets=%llu",
                 a->peak_pending_frames * 1000u / AUDIO_RATE,
                 a->max_packet_frames * 1000u / a->format.nSamplesPerSec,
                 (unsigned long long)a->overflow_packets);
        psrdp_diag_line(line);
    }
    a->read = a->count = 0;
    a->have_previous = 0;
}

static BOOL audio_open(rdpsndDevicePlugin *device, const AUDIO_FORMAT *format, UINT32 latency)
{
    psrdpAudio *a = (psrdpAudio *)device;
    (void)latency; /* The bounded ring sets the maximum local queue latency. */
    audio_close(device);
    if (!audio_format_supported(device, format)) {
        audio_log("unsupported PCM format", format ? format->wFormatTag : -1);
        return FALSE;
    }
    a->format = *format;
    a->format.data = NULL;
    a->received = a->submitted = a->dropped = a->underruns = a->malformed = 0;
    a->max_packet_frames = a->peak_pending_frames = 0;
    a->overflow_packets = 0;
    a->quiet_frames = AUDIO_IDLE_FRAMES;
    a->empty_frames = a->onset_logs = 0;
    a->onset_pending = 0;
    a->failed = a->started = a->phase = 0;
    a->output_logged = a->discard_logged = 0;
    /* Init may report already initialized if a previous session used audio.
     * Open is authoritative; never change system/user/HDMI configuration. */
    int init_rc = sceAudioOutInit();
    a->port = sceAudioOutOpen(0xff, 0 /* MAIN */, 0, AUDIO_GRAIN, AUDIO_RATE,
                             1 /* signed 16-bit stereo */);
    if (a->port < 0) {
        audio_log("init result", init_rc);
        audio_log("main port open failed; using silent playback", a->port);
        /* Rejecting a negotiated stream makes rdpsnd report a session error.
         * Keep acknowledging it while audio_play discards the samples. */
        return TRUE;
    }
    a->running = 1;
    int rc = pthread_create(&a->thread, NULL, audio_output, a);
    if (rc) {
        a->running = 0;
        audio_log("output thread failed; using silent playback", rc);
        audio_close(device);
        return TRUE;
    }
    a->thread_started = 1;
    char line[200];
    snprintf(line, sizeof(line), "audio: PCM %u Hz %u ch %u bit -> main/TV 48000 Hz stereo S16; grain=256 queue capacity=2000ms immediate start",
             format->nSamplesPerSec, format->nChannels, format->wBitsPerSample);
    psrdp_diag_line(line);
    return TRUE;
}

static int16_t audio_sample(const BYTE *p, unsigned bits)
{
    if (bits == 8) return (int16_t)(((int)p[0] - 128) * 256);
    /* Integer PCM is little-endian; truncate low bits for the S16 port. */
    unsigned off = bits / 8u - 2u;
    unsigned u = (unsigned)p[off] | ((unsigned)p[off + 1] << 8);
    return (int16_t)(u >= 32768u ? (int)u - 65536 : (int)u);
}

/* Caller holds lock. Keep samples in playback order even during a burst. */
static void audio_push(psrdpAudio *a, int16_t left, int16_t right, uint64_t packet_ms)
{
    const int signal = left || right;
    const int was_idle = a->quiet_frames >= AUDIO_IDLE_FRAMES;
    if (signal) a->quiet_frames = 0;
    else if (!was_idle) a->quiet_frames++;

    if (a->count == AUDIO_CAPACITY) {
        /* Sustained oversupply exhausted the safety limit. Preserve queued audio. */
        a->dropped++;
        return;
    }
    if (signal && was_idle && a->onset_logs < 8 && !a->onset_pending && packet_ms) {
        a->onset_offset = a->count;
        a->onset_ms = packet_ms;
        a->onset_pending = 1;
    }
    size_t w = (a->read + a->count) % AUDIO_CAPACITY;
    a->ring[w][0] = left;
    a->ring[w][1] = right;
    a->count++;
}

static UINT audio_play(rdpsndDevicePlugin *device, const BYTE *data, size_t size)
{
    psrdpAudio *a = (psrdpAudio *)device;
    const uint64_t packet_ms = audio_now_ms();
    pthread_mutex_lock(&a->lock);
    if (!a->running || a->failed || !a->format.nBlockAlign) {
        int first_discard = size && !a->discard_logged;
        if (first_discard) a->discard_logged = 1;
        pthread_mutex_unlock(&a->lock);
        if (first_discard) audio_log("received audio with main output unavailable; bytes", (int)size);
        return 0;
    }
    /* A truncated interleaved frame must not shift channel alignment. */
    if ((!data && size) || size % a->format.nBlockAlign || size > 1024u * 1024u) {
        a->malformed++;
        pthread_mutex_unlock(&a->lock);
        return 0;
    }
    unsigned bytes = a->format.wBitsPerSample / 8u;
    size_t frames = size / a->format.nBlockAlign;
    if (frames > a->max_packet_frames) a->max_packet_frames = frames;
    const uint64_t dropped_before = a->dropped;
    for (size_t i = 0; i < frames; i++) {
        const BYTE *p = data + i * a->format.nBlockAlign;
        int16_t sample[2];
        sample[0] = audio_sample(p, a->format.wBitsPerSample);
        sample[1] = a->format.nChannels == 1 ? sample[0] : audio_sample(p + bytes, a->format.wBitsPerSample);
        if (!a->have_previous) {
            audio_push(a, sample[0], sample[1], packet_ms);
            a->have_previous = 1;
            a->phase = a->format.nSamplesPerSec;
        } else {
            /* Linear interpolation; phase persists across network packets. */
            while (a->phase <= AUDIO_RATE) {
                int16_t l = (int16_t)(a->previous[0] +
                    (int64_t)(sample[0] - a->previous[0]) * a->phase / AUDIO_RATE);
                int16_t r = (int16_t)(a->previous[1] +
                    (int64_t)(sample[1] - a->previous[1]) * a->phase / AUDIO_RATE);
                audio_push(a, l, r, packet_ms);
                a->phase += a->format.nSamplesPerSec;
            }
            a->phase -= AUDIO_RATE;
        }
        a->previous[0] = sample[0];
        a->previous[1] = sample[1];
    }
    if (a->count > a->peak_pending_frames) a->peak_pending_frames = a->count;
    if (a->dropped != dropped_before) a->overflow_packets++;
    a->received += frames;
    if (frames) pthread_cond_signal(&a->wake);
    UINT delay = (UINT)((a->count * 1000u + AUDIO_RATE - 1u) / AUDIO_RATE);
    /* Include at most one in-flight output grain in the confirm estimate. */
    if (a->started) delay += (AUDIO_GRAIN * 1000u + AUDIO_RATE - 1u) / AUDIO_RATE;
    pthread_mutex_unlock(&a->lock);
    return delay; /* FreeRDP expects milliseconds, NOT a channel status. */
}

static UINT32 audio_get_volume(rdpsndDevicePlugin *device)
{
    psrdpAudio *a = (psrdpAudio *)device;
    pthread_mutex_lock(&a->lock);
    UINT32 volume = a->volume;
    pthread_mutex_unlock(&a->lock);
    return volume;
}

static BOOL audio_set_volume(rdpsndDevicePlugin *device, UINT32 volume)
{
    psrdpAudio *a = (psrdpAudio *)device;
    pthread_mutex_lock(&a->lock);
    a->volume = volume;
    pthread_mutex_unlock(&a->lock);
    return TRUE;
}

static UINT audio_announce(rdpsndDevicePlugin *device, const AUDIO_FORMAT *formats, size_t count)
{
    size_t pcm = 0, adpcm = 0;
    for (size_t i = 0; i < count; ++i) {
        AUDIO_FORMAT decoded;
        if (audio_format_supported(device, &formats[i])) ++pcm;
        else if (audio_default_format(device, &formats[i], &decoded)) ++adpcm;
    }
    char line[160];
    snprintf(line, sizeof(line), "audio: server formats=%zu compatible PCM=%zu ADPCM=%zu", count, pcm, adpcm);
    psrdp_diag_line(line);
    return CHANNEL_RC_OK;
}

static void audio_free(rdpsndDevicePlugin *device)
{
    psrdpAudio *a = (psrdpAudio *)device;
    if (!a) return;
    audio_close(device);
    pthread_cond_destroy(&a->wake);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

static UINT VCAPITYPE audio_device_entry(PFREERDP_RDPSND_DEVICE_ENTRY_POINTS ep)
{
    if (!ep || !ep->pRegisterRdpsndDevice) return ERROR_INVALID_PARAMETER;
    psrdpAudio *a = calloc(1, sizeof(*a));
    if (!a) return CHANNEL_RC_NO_MEMORY;
    a->port = -1;
    a->volume = 0xffffffffu;
    if (pthread_mutex_init(&a->lock, NULL)) { free(a); return CHANNEL_RC_NO_MEMORY; }
    if (pthread_cond_init(&a->wake, NULL)) {
        pthread_mutex_destroy(&a->lock); free(a); return CHANNEL_RC_NO_MEMORY;
    }
    a->device.FormatSupported = audio_format_supported;
    a->device.DefaultFormat = audio_default_format;
    a->device.Open = audio_open;
    a->device.GetVolume = audio_get_volume;
    a->device.SetVolume = audio_set_volume;
    a->device.Play = audio_play;
    a->device.Close = audio_close;
    a->device.Free = audio_free;
    a->device.ServerFormatAnnounce = audio_announce;
    ep->pRegisterRdpsndDevice(ep->rdpsnd, &a->device);
    psrdp_diag_line("audio: PS5 backend registered (no capture)");
    return CHANNEL_RC_OK;
}

static FREERDP_LOAD_CHANNEL_ADDIN_ENTRY_FN previous_provider;
static pthread_once_t provider_once = PTHREAD_ONCE_INIT;
static int provider_status = -1;

static PVIRTUALCHANNELENTRY audio_provider(LPCSTR name, LPCSTR subsystem, LPCSTR type, DWORD flags)
{
    if (name && subsystem && strcmp(name, "rdpsnd") == 0 && strcmp(subsystem, "ps5") == 0) {
        union { PFREERDP_RDPSND_DEVICE_ENTRY audio; PVIRTUALCHANNELENTRY generic; } entry;
        entry.audio = audio_device_entry;
        return entry.generic;
    }
    return previous_provider ? previous_provider(name, subsystem, type, flags) : NULL;
}

static void audio_register_provider(void)
{
    previous_provider = freerdp_get_current_addin_provider();
    provider_status = freerdp_register_addin_provider(audio_provider, 0);
}

extern BOOL VCAPITYPE rdpdr_VirtualChannelEntryEx(PCHANNEL_ENTRY_POINTS_EX entry, PVOID init);

BOOL psrdp_audio_attach(freerdp *instance)
{
    /* Static storage is required: FreeRDP borrows this until context teardown. */
    static char arg0[] = "rdpsnd", arg1[] = "sys:ps5", arg2[] = "latency:80";
    /* Allow Windows to choose its stream quality rather than request high quality. */
    static char arg3[] = "quality:dynamic";
    static char *argv[] = { arg0, arg1, arg2, arg3 };
    static ADDIN_ARGV args = { 4, argv };
    if (!instance || !instance->context || !instance->context->settings || !instance->context->channels)
        return FALSE;
    pthread_once(&provider_once, audio_register_provider);
    if (provider_status) return FALSE;
    rdpSettings *settings = instance->context->settings;
    if (!freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteConsoleAudio, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, FALSE)) return FALSE;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, TRUE)) return FALSE;
    /* The PS5 system hostname can be empty. Supply a nonempty client name
     * before RDPDR initializes and sends its Client Name Request. */
    const char *name = freerdp_settings_get_string(settings, FreeRDP_ClientHostname);
    if ((!name || !name[0]) &&
        !freerdp_settings_set_string(settings, FreeRDP_ClientHostname, "PSrdp"))
        return FALSE;
    /* RDPDR completes the server handshake with an empty device list. */
    int rc = freerdp_channels_client_load_ex(instance->context->channels, settings,
                                            rdpdr_VirtualChannelEntryEx, settings);
    audio_log("rdpdr channel load", rc);
    if (rc != 0) return FALSE;
    rc = freerdp_channels_client_load_ex(instance->context->channels, settings,
                                            rdpsnd_VirtualChannelEntryEx, &args);
    audio_log("rdpsnd channel load", rc);
    return rc == 0;
}
