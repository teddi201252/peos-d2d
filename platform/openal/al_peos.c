// OpenAL on PeOS (d2decomped port): the calls a game uses as a mixer, mixed
// by a thread of its own into a stream of PeOS's audio (peos_audio_open: the
// kernel sums every program's stream, 48 kHz stereo 16-bit).
//
// Sources play their buffers (or a queue of them) resampled linearly to
// 48 kHz, at their gain; a mono source is panned by its position (constant
// power, the centre at -3 dB as openal-soft has it) and attenuated by its
// distance (OpenAL's default: inverse, clamped, reference 1, rolloff 1); a
// stereo one goes straight to the speakers (AL_SOFT_direct_channels, which
// d2d asks for to keep game.exe's own pan law). A source's time is what
// the mixer has played of it: its processed buffers, its sample offset.
//
// Without a device (no "audio" capability, no sound card) the same thread
// keeps the time with the clock, playing to nobody: a game that waits on
// its sources goes on all the same.
#include <AL/al.h>
#include <AL/alc.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <peos/peos.h>

#define OUT_RATE 48000u
#define PERIOD 1024u            // frames mixed at a time: ~21 ms
#define QUEUE_MAX 64

typedef struct {
    int used;
    ALenum format;
    ALsizei freq;
    ALsizei size;
    uint8_t *data;
} buffer_t;

typedef struct {
    int used;
    ALint state;
    ALfloat gain, pitch;
    ALfloat pos[3];
    ALint looping, relative, direct;
    ALuint queue[QUEUE_MAX];
    int queued, processed;   // the queue's buffers, and how many are done
    uint64_t cursor;         // in the current buffer: frames, 32.32 fixed point
} source_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ALenum g_error = AL_NO_ERROR;
static source_t *g_sources;
static int g_nsources;
static buffer_t *g_buffers;
static int g_nbuffers;

static void set_error(ALenum e) {
    if (g_error == AL_NO_ERROR) g_error = e;
}

static source_t *source(ALuint id) {
    if (id == 0 || (int)id > g_nsources || !g_sources[id - 1].used) {
        set_error(AL_INVALID_NAME);
        return NULL;
    }
    return &g_sources[id - 1];
}

static buffer_t *buffer(ALuint id) {
    if (id == 0 || (int)id > g_nbuffers || !g_buffers[id - 1].used) return NULL;
    return &g_buffers[id - 1];
}

static int channels_of(ALenum format) {
    return format == AL_FORMAT_STEREO8 || format == AL_FORMAT_STEREO16 ? 2 : 1;
}

static int bytes_of(ALenum format) {
    return format == AL_FORMAT_MONO16 || format == AL_FORMAT_STEREO16 ? 2 : 1;
}

static int frame_bytes(ALenum format) {
    switch (format) {
        case AL_FORMAT_MONO8: case AL_FORMAT_MONO16: case AL_FORMAT_STEREO8: case AL_FORMAT_STEREO16:
            return channels_of(format) * bytes_of(format);
        default:
            return 0;
    }
}

static uint32_t frames_of(const buffer_t *b) {
    int fb = frame_bytes(b->format);
    return fb ? (uint32_t)b->size / (uint32_t)fb : 0u;
}

// one channel of one frame as -1..1
static float sample_at(const buffer_t *b, uint32_t frame, int channel) {
    int ch = channels_of(b->format);
    if (ch == 1) channel = 0;
    const uint8_t *p = b->data + ((size_t)frame * (size_t)ch + (size_t)channel) * (size_t)bytes_of(b->format);
    if (bytes_of(b->format) == 1) return ((float)p[0] - 128.0f) / 128.0f;
    return (float)(int16_t)(p[0] | p[1] << 8) / 32768.0f;
}

// ---- the mixer ----

static pthread_t g_mixer;
static int g_mixer_running;
static volatile int g_mixer_stop;
static int64_t g_audio_fd = -1;

// A source's left and right gains: its gain, and for a mono source the pan
// and the distance of its position.
static void gains(const source_t *s, int stereo, float *left, float *right) {
    float g = s->gain;
    if (stereo || s->direct) {
        *left = *right = g;
        return;
    }
    float x = s->pos[0], y = s->pos[1], z = s->pos[2];
    float dist = sqrtf(x * x + y * y + z * z);
    float pan = dist > 1e-6f ? x / dist : 0.0f;
    if (dist > 1.0f) g /= dist;   // inverse distance, clamped at the reference 1
    float theta = (pan + 1.0f) * 0.78539816f;   // 0..pi/2
    *left = g * cosf(theta);
    *right = g * sinf(theta);
}

// Plays `n` frames of the source into `out` (interleaved L, R), moving its
// time on; stops it at its end unless it loops.
static void mix_source(source_t *s, float *out, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        if (s->processed >= s->queued) {
            if (s->looping && s->queued > 0) {
                s->processed = 0;
                s->cursor = 0;
                continue;
            }
            s->state = AL_STOPPED;
            return;
        }
        buffer_t *b = buffer(s->queue[s->processed]);
        uint32_t frames = b ? frames_of(b) : 0u;
        if (!b || !frames || b->freq <= 0) {   // nothing to play here: on to the next
            s->processed++;
            s->cursor = 0;
            continue;
        }
        int stereo = channels_of(b->format) == 2;
        float gl, gr;
        gains(s, stereo, &gl, &gr);
        float pitch = s->pitch > 0.0f ? s->pitch : 1.0f;
        uint64_t step = (uint64_t)((double)b->freq * pitch / OUT_RATE * 4294967296.0);
        if (!step) step = 1;
        uint64_t end = (uint64_t)frames << 32;
        while (i < n && s->cursor < end) {
            uint32_t f = (uint32_t)(s->cursor >> 32);
            float t = (float)(uint32_t)s->cursor / 4294967296.0f;
            uint32_t f1 = f + 1 < frames ? f + 1 : f;
            float l = sample_at(b, f, 0), l1 = sample_at(b, f1, 0);
            float l_out = l + (l1 - l) * t, r_out = l_out;
            if (stereo) {
                float r = sample_at(b, f, 1), r1 = sample_at(b, f1, 1);
                r_out = r + (r1 - r) * t;
            }
            out[2 * i] += l_out * gl;
            out[2 * i + 1] += r_out * gr;
            s->cursor += step;
            i++;
        }
        if (s->cursor >= end) {
            s->cursor -= end;
            if (s->looping && s->queued == 1) continue;   // a looped buffer: round again
            s->processed++;
            if (s->processed >= s->queued) s->cursor = 0;
        }
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t) {
    uint64_t now = now_ns();
    if (t <= now) return;
    struct timespec ts = { (time_t)((t - now) / 1000000000ull), (long)((t - now) % 1000000000ull) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static void *mixer(void *arg) {
    (void)arg;
    static float mix[PERIOD * 2];
    static int16_t pcm[PERIOD * 2];
    const uint64_t period_ns = (uint64_t)PERIOD * 1000000000ull / OUT_RATE;
    uint64_t next = now_ns();
    while (!g_mixer_stop) {
        memset(mix, 0, sizeof(mix));
        int playing = 0;
        pthread_mutex_lock(&g_lock);
        for (int k = 0; k < g_nsources; k++) {
            source_t *s = &g_sources[k];
            if (!s->used || s->state != AL_PLAYING) continue;
            mix_source(s, mix, PERIOD);
            playing = 1;
        }
        pthread_mutex_unlock(&g_lock);
        if (!playing) {   // nothing to play: no silence sent, the device rests
            next = now_ns() + 5000000ull;
            sleep_until(next);
            next = now_ns();
            continue;
        }
        for (uint32_t i = 0; i < PERIOD * 2; i++) {
            float v = mix[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = (int16_t)lrintf(v * 32767.0f);
        }
        if (g_audio_fd >= 0) {
            // the write waits for the device: it is the mixer's clock
            if (peos_fwrite((uint64_t)g_audio_fd, pcm, sizeof(pcm)) != sizeof(pcm)) {
                g_audio_fd = -1;   // the device went: keep the time with the clock
                next = now_ns();
            }
        } else {
            next += period_ns;
            sleep_until(next);
        }
    }
    return NULL;
}

// ---- AL ----

ALenum alGetError(void) {
    pthread_mutex_lock(&g_lock);
    ALenum e = g_error;
    g_error = AL_NO_ERROR;
    pthread_mutex_unlock(&g_lock);
    return e;
}

const ALchar *alGetString(ALenum param) {
    switch (param) {
        case AL_VENDOR: return "PeOS";
        case AL_VERSION: return "1.1 PeOS";
        case AL_RENDERER: return "PeOS mixer";
        case AL_EXTENSIONS: return "AL_SOFT_direct_channels";
        default: return NULL;
    }
}

ALboolean alIsExtensionPresent(const ALchar *extname) {
    return extname && !strcmp(extname, "AL_SOFT_direct_channels") ? AL_TRUE : AL_FALSE;
}

void alGenSources(ALsizei n, ALuint *sources) {
    pthread_mutex_lock(&g_lock);
    for (ALsizei i = 0; i < n; i++) {
        int at = -1;
        for (int k = 0; k < g_nsources; k++)
            if (!g_sources[k].used) { at = k; break; }
        if (at < 0) {
            source_t *grown = realloc(g_sources, (size_t)(g_nsources + 16) * sizeof(source_t));
            if (!grown) { set_error(AL_OUT_OF_MEMORY); break; }
            memset(grown + g_nsources, 0, 16 * sizeof(source_t));
            g_sources = grown;
            at = g_nsources;
            g_nsources += 16;
        }
        source_t *s = &g_sources[at];
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->state = AL_INITIAL;
        s->gain = 1.0f;
        s->pitch = 1.0f;
        sources[i] = (ALuint)at + 1u;
    }
    pthread_mutex_unlock(&g_lock);
}

void alDeleteSources(ALsizei n, const ALuint *sources) {
    pthread_mutex_lock(&g_lock);
    for (ALsizei i = 0; i < n; i++) {
        source_t *s = source(sources[i]);
        if (s) s->used = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

static uint64_t current_freq(const source_t *s) {
    const buffer_t *b = s->processed < s->queued ? buffer(s->queue[s->processed]) : NULL;
    return b && b->freq > 0 ? (uint64_t)b->freq : 0u;
}

void alSourcef(ALuint id, ALenum param, ALfloat value) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s) {
        if (param == AL_GAIN) s->gain = value < 0.0f ? 0.0f : value;
        else if (param == AL_PITCH) s->pitch = value;
        else if (param == AL_SEC_OFFSET) {
            uint64_t freq = current_freq(s);
            if (freq) s->cursor = (uint64_t)((double)value * (double)freq * 4294967296.0);
        } else set_error(AL_INVALID_ENUM);
    }
    pthread_mutex_unlock(&g_lock);
}

void alSource3f(ALuint id, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s) {
        if (param == AL_POSITION) {
            s->pos[0] = v1;
            s->pos[1] = v2;
            s->pos[2] = v3;
        } else if (param != AL_VELOCITY) set_error(AL_INVALID_ENUM);
    }
    pthread_mutex_unlock(&g_lock);
}

void alSourcei(ALuint id, ALenum param, ALint value) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s) {
        switch (param) {
            case AL_BUFFER:   // one buffer, or none
                if (s->state == AL_PLAYING || s->state == AL_PAUSED) { set_error(AL_INVALID_OPERATION); break; }
                s->queued = 0;
                s->processed = 0;
                s->cursor = 0;
                if (value) {
                    if (!buffer((ALuint)value)) { set_error(AL_INVALID_VALUE); break; }
                    s->queue[s->queued++] = (ALuint)value;
                }
                break;
            case AL_LOOPING: s->looping = value; break;
            case AL_SOURCE_RELATIVE: s->relative = value; break;
            case AL_SAMPLE_OFFSET: s->cursor = (uint64_t)(value < 0 ? 0 : value) << 32; break;
            case 0x1033: s->direct = value; break;   // AL_DIRECT_CHANNELS_SOFT
            default: set_error(AL_INVALID_ENUM); break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void alGetSourcef(ALuint id, ALenum param, ALfloat *value) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s && value) {
        if (param == AL_GAIN) *value = s->gain;
        else if (param == AL_PITCH) *value = s->pitch;
        else if (param == AL_SEC_OFFSET) {
            uint64_t freq = current_freq(s);
            *value = freq && (s->state == AL_PLAYING || s->state == AL_PAUSED)
                         ? (ALfloat)((double)s->cursor / 4294967296.0 / (double)freq) : 0.0f;
        } else set_error(AL_INVALID_ENUM);
    }
    pthread_mutex_unlock(&g_lock);
}

void alGetSourcei(ALuint id, ALenum param, ALint *value) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s && value) {
        switch (param) {
            case AL_SOURCE_STATE: *value = s->state; break;
            case AL_BUFFERS_QUEUED: *value = s->queued; break;
            case AL_BUFFERS_PROCESSED: *value = s->state == AL_STOPPED ? s->queued : s->processed; break;
            case AL_LOOPING: *value = s->looping; break;
            case AL_SOURCE_RELATIVE: *value = s->relative; break;
            case AL_BUFFER: *value = s->queued ? (ALint)s->queue[s->processed < s->queued ? s->processed : s->queued - 1] : 0; break;
            case AL_SAMPLE_OFFSET:
                *value = s->state == AL_PLAYING || s->state == AL_PAUSED ? (ALint)(s->cursor >> 32) : 0;
                break;
            default: set_error(AL_INVALID_ENUM); break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void play(source_t *s) {
    if (s->state != AL_PAUSED) {   // from the start: every buffer again (OpenAL rewinds a playing one too)
        s->processed = 0;
        s->cursor = 0;
    }
    s->state = s->queued ? AL_PLAYING : AL_STOPPED;
}

void alSourcePlay(ALuint id) { alSourcePlayv(1, &id); }

void alSourcePlayv(ALsizei n, const ALuint *sources) {
    pthread_mutex_lock(&g_lock);
    for (ALsizei i = 0; i < n; i++) {
        source_t *s = source(sources[i]);
        if (s) play(s);
    }
    pthread_mutex_unlock(&g_lock);
}

void alSourcePause(ALuint id) { alSourcePausev(1, &id); }

void alSourcePausev(ALsizei n, const ALuint *sources) {
    pthread_mutex_lock(&g_lock);
    for (ALsizei i = 0; i < n; i++) {
        source_t *s = source(sources[i]);
        if (s && s->state == AL_PLAYING) s->state = AL_PAUSED;
    }
    pthread_mutex_unlock(&g_lock);
}

void alSourceStop(ALuint id) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s) {
        s->state = AL_STOPPED;
        s->processed = s->queued;   // all of them done, as OpenAL says
        s->cursor = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void alSourceQueueBuffers(ALuint id, ALsizei nb, const ALuint *buffers) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s) {
        // a stopped source that is given more starts its queue anew
        if (s->state == AL_STOPPED && s->processed > 0) {
            memmove(s->queue, s->queue + s->processed, (size_t)(s->queued - s->processed) * sizeof(ALuint));
            s->queued -= s->processed;
            s->processed = 0;
            s->cursor = 0;
        }
        if (s->queued + nb > QUEUE_MAX) set_error(AL_INVALID_OPERATION);
        else {
            for (ALsizei i = 0; i < nb; i++) {
                if (!buffer(buffers[i])) { set_error(AL_INVALID_NAME); break; }
                s->queue[s->queued++] = buffers[i];
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void alSourceUnqueueBuffers(ALuint id, ALsizei nb, ALuint *buffers) {
    pthread_mutex_lock(&g_lock);
    source_t *s = source(id);
    if (s) {
        int done = s->state == AL_STOPPED ? s->queued : s->processed;
        if (nb > done) set_error(AL_INVALID_VALUE);
        else {
            for (ALsizei i = 0; i < nb; i++) buffers[i] = s->queue[i];
            memmove(s->queue, s->queue + nb, (size_t)(s->queued - nb) * sizeof(ALuint));
            s->queued -= nb;
            s->processed = s->processed > nb ? s->processed - nb : 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void alGenBuffers(ALsizei n, ALuint *buffers) {
    pthread_mutex_lock(&g_lock);
    for (ALsizei i = 0; i < n; i++) {
        int at = -1;
        for (int k = 0; k < g_nbuffers; k++)
            if (!g_buffers[k].used) { at = k; break; }
        if (at < 0) {
            buffer_t *grown = realloc(g_buffers, (size_t)(g_nbuffers + 64) * sizeof(buffer_t));
            if (!grown) { set_error(AL_OUT_OF_MEMORY); break; }
            memset(grown + g_nbuffers, 0, 64 * sizeof(buffer_t));
            g_buffers = grown;
            at = g_nbuffers;
            g_nbuffers += 64;
        }
        memset(&g_buffers[at], 0, sizeof(buffer_t));
        g_buffers[at].used = 1;
        buffers[i] = (ALuint)at + 1u;
    }
    pthread_mutex_unlock(&g_lock);
}

// a buffer still in some source's queue cannot go (OpenAL: invalid operation)
static int buffer_in_use(ALuint id) {
    for (int k = 0; k < g_nsources; k++) {
        const source_t *s = &g_sources[k];
        if (!s->used) continue;
        for (int q = 0; q < s->queued; q++)
            if (s->queue[q] == id) return 1;
    }
    return 0;
}

void alDeleteBuffers(ALsizei n, const ALuint *buffers) {
    pthread_mutex_lock(&g_lock);
    for (ALsizei i = 0; i < n; i++) {
        buffer_t *b = buffer(buffers[i]);
        if (!b) {
            if (buffers[i]) set_error(AL_INVALID_NAME);
            continue;
        }
        if (buffer_in_use(buffers[i])) {
            set_error(AL_INVALID_OPERATION);
            continue;
        }
        free(b->data);
        memset(b, 0, sizeof(*b));
    }
    pthread_mutex_unlock(&g_lock);
}

void alBufferData(ALuint id, ALenum format, const ALvoid *data, ALsizei size, ALsizei samplerate) {
    pthread_mutex_lock(&g_lock);
    buffer_t *b = buffer(id);
    if (!b) set_error(AL_INVALID_NAME);
    else if (!frame_bytes(format) || size < 0 || samplerate <= 0) set_error(AL_INVALID_VALUE);
    else if (buffer_in_use(id)) set_error(AL_INVALID_OPERATION);
    else {
        uint8_t *copy = malloc(size ? (size_t)size : 1u);
        if (!copy) set_error(AL_OUT_OF_MEMORY);
        else {
            if (size) memcpy(copy, data, (size_t)size);
            free(b->data);
            b->data = copy;
            b->format = format;
            b->size = size;
            b->freq = samplerate;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- the device and the context: one each ----

struct ALCdevice {
    int open;
};
struct ALCcontext {
    ALCdevice *device;
};

static ALCdevice g_device;
static ALCcontext g_context;

ALCdevice *alcOpenDevice(const ALCchar *devicename) {
    (void)devicename;
    if (g_device.open) return &g_device;
    g_audio_fd = peos_audio_open();   // -1 without "audio" or a sound card: kept in time all the same
    g_mixer_stop = 0;
    g_mixer_running = pthread_create(&g_mixer, NULL, mixer, NULL) == 0;
    g_device.open = 1;
    return &g_device;
}

ALCboolean alcCloseDevice(ALCdevice *device) {
    if (device != &g_device || !g_device.open) return ALC_FALSE;
    g_mixer_stop = 1;
    if (g_mixer_running) pthread_join(g_mixer, NULL);
    g_mixer_running = 0;
    if (g_audio_fd >= 0) (void)peos_fclose((uint64_t)g_audio_fd);
    g_audio_fd = -1;
    g_device.open = 0;
    return ALC_TRUE;
}

ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist) {
    (void)attrlist;
    if (device != &g_device) return NULL;
    g_context.device = device;
    return &g_context;
}

void alcDestroyContext(ALCcontext *context) {
    if (context == &g_context) g_context.device = NULL;
}

ALCboolean alcMakeContextCurrent(ALCcontext *context) {
    return context == NULL || context == &g_context ? ALC_TRUE : ALC_FALSE;
}

const ALCchar *alcGetString(ALCdevice *device, ALCenum param) {
    (void)device;
    if (param == ALC_DEVICE_SPECIFIER || param == ALC_DEFAULT_DEVICE_SPECIFIER) return "PeOS audio";
    return "";
}
