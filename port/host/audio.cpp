// Sound output (host.h). The game's AI buffers go into a ring that SDL's stream callback drains.
// Playback starts once the ring holds `target` frames, and the callback steps through the ring a
// little faster or slower (at most 0.5%, inaudible) to keep it near that fill: the game is paced by
// the host clock and the sound card by its own, and a hitch on either side is absorbed instead of
// heard as a click. If the ring runs dry anyway, the output fades the last frame out and waits to
// refill. (Moved from eltale_rt64.dll as it was; SDL3's stream takes the callback's output.)
#include "host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace host {
namespace {

constexpr uint32_t AUDIO_RING = 1 << 15; // stereo frames (about 1.5 s at 22 kHz)
constexpr double AUDIO_MAX_STEP = 0.005;

struct AudioRing {
    int16_t frames[AUDIO_RING][2];
    uint64_t read_pos, write_pos; // in frames; read_pos is advanced by the callback
    double frac;                  // position between read_pos and read_pos + 1
    uint32_t target;              // fill to start at and to steer towards
    bool playing;
    float last[2];
} ring;

SDL_AudioStream *stream;
uint32_t stream_rate;
uint64_t queued, played; // frames, over every stream (the log at the end)
std::vector<int16_t> out;

// The sound's volume now, 0..1 (the setting; muted in the background)
float volume() {
    if (settings.mute_in_background && !focused()) return 0.0f;
    return settings.volume / 100.0f;
}

// SDL asks for `additional` more bytes (the stream is locked while this runs)
void SDLCALL callback(void *, SDL_AudioStream *s, int additional, int) {
    const int count = additional / 4;
    if (count <= 0) return;
    out.resize(size_t(count) * 2);
    const float gain = volume();
    uint64_t avail = ring.write_pos - ring.read_pos;
    if (!ring.playing && avail >= ring.target) ring.playing = true;
    double step = 1.0;
    if (ring.playing) {
        double error = ((double)avail - ring.target) / ring.target;
        step = 1.0 + std::max(-AUDIO_MAX_STEP, std::min(AUDIO_MAX_STEP, error * AUDIO_MAX_STEP));
    }
    for (int i = 0; i < count; i++) {
        if (ring.playing && ring.write_pos - ring.read_pos >= 2) {
            const int16_t *a = ring.frames[ring.read_pos % AUDIO_RING], *b = ring.frames[(ring.read_pos + 1) % AUDIO_RING];
            for (int c = 0; c < 2; c++) {
                ring.last[c] = (float)(a[c] + (b[c] - a[c]) * ring.frac) * gain;
                out[i * 2 + c] = (int16_t)ring.last[c];
            }
            ring.frac += step;
            while (ring.frac >= 1.0) { ring.frac -= 1.0; ring.read_pos++; }
        } else {
            ring.playing = false; // underrun: fade out over a few ms and refill
            for (int c = 0; c < 2; c++) {
                ring.last[c] *= 0.97f;
                out[i * 2 + c] = (int16_t)ring.last[c];
            }
        }
    }
    SDL_PutAudioStreamData(s, out.data(), count * 4);
}

} // namespace

void audio_queue(const int16_t *samples, uint32_t bytes, uint32_t rate) {
    if (rate != stream_rate) {
        if (stream) {
            played += ring.read_pos;
            SDL_DestroyAudioStream(stream);
        }
        stream = nullptr;
        stream_rate = rate;
        ring = {};
        ring.target = rate / 15; // about 67 ms: four game frames of sound
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
        const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, (int)rate };
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, nullptr);
        if (stream) SDL_ResumeAudioStreamDevice(stream);
        else std::fprintf(stderr, "[host] audio: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
    }
    if (!stream) return;
    uint32_t count = bytes / 4;
    SDL_LockAudioStream(stream);
    // far too much queued (the game ran ahead after a stall): drop the oldest down to the target
    if (ring.write_pos - ring.read_pos + count > AUDIO_RING / 2) ring.read_pos = ring.write_pos + count - ring.target;
    for (uint32_t i = 0; i < count; i++) {
        ring.frames[ring.write_pos % AUDIO_RING][0] = samples[i * 2];
        ring.frames[ring.write_pos % AUDIO_RING][1] = samples[i * 2 + 1];
        ring.write_pos++;
    }
    queued += count;
    SDL_UnlockAudioStream(stream);
}

void audio_shutdown() {
    if (stream) {
        SDL_LockAudioStream(stream);
        played += ring.read_pos;
        SDL_UnlockAudioStream(stream);
        SDL_DestroyAudioStream(stream);
    }
    // (the hidden-window test reads this: sound was queued and the device took it)
    std::fprintf(stderr, "HOST_AUDIO queued=%llu played=%llu rate=%u\n", (unsigned long long)queued,
                 (unsigned long long)played, stream_rate);
    stream = nullptr;
    stream_rate = 0;
}

} // namespace host
