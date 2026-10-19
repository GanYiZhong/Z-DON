#pragma once

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include "av.h"
#include "config.h"
#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
#include <RtAudio.h>
#endif
#ifdef _WIN32
#include <portaudio.h>
#endif
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <samplerate.h>
#include <shared_mutex>
#include <sndfile.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace song_loudness {
    class Scanner;
}

enum class VolumePreset {
    NONE,
    SOUND,
    MUSIC,
    VOICE,
    HITSOUND,
    ATTRACT_MODE,
};

struct VirtualFile {
    std::shared_ptr<const std::vector<uint8_t>> data;
    sf_count_t pos = 0;
};

// A long sound whose tail is still being decoded on a background thread (see load_sound):
// the mixer plays the frames that are ready.
struct SoundDecodeJob {
    std::atomic<unsigned int> frames_ready{0}; // frames of sound::data written so far
    std::atomic<bool> done{false};             // frames_ready is the sound's final length
    std::atomic<bool> cancel{false};
    // Ownership of sound::data while the decoder runs: an unload that finds the decoder still
    // running hands the buffer over (orphaned) instead of waiting, and the decoder frees it on
    // exit. Both sides take this lock only for these flags, never while decoding.
    std::mutex state;
    bool decoder_exited = false;
    float* orphaned = nullptr;
};

// A decoded sound as a loader hands it to store_sound. Once stored, the PCM part (data,
// frame_count, sample_rate, channels, normalization_gain, decode_job) is immutable and owned by
// the engine; the playback part seeds the sound's slot.
struct sound {
    float* data = nullptr;        // Audio sample data (interleaved stereo or mono)
    unsigned int frame_count = 0; // Total number of frames in the sound
    unsigned int sample_rate = 0; // Original sample rate of the audio
    unsigned int channels = 0;    // Number of channels (1 = mono, 2 = stereo)

    bool is_playing = false;        // Whether the sound is currently playing
    unsigned int current_frame = 0; // Current playback position in frames
    float frame_frac = 0.0f;        // Fractional part of playback position (for pitch != 1.0)
    bool loop = false;              // Whether to loop the sound

    float normalization_gain = 1.0f;

    float volume = 1.0f; // Volume multiplier (0.0 to 1.0+)
    float playback_gain = 1.0f;
    float pan = 0.5f;   // Stereo pan (0.0 = left, 0.5 = center, 1.0 = right)
    float pitch = 1.0f; // Pitch/speed multiplier (1.0 = normal)

    SRC_STATE* resampler = nullptr;   // libsamplerate state (if needed)
    float* resample_buffer = nullptr; // Buffer for resampled audio

    std::shared_ptr<SoundDecodeJob> decode_job; // set while (or after) the tail decodes in the background
};

// Stable handle of a sound name (Z-DON): resolve once with AudioEngine::sound_id, play with no
// string hashing and no lock. A handle stays valid for the engine's lifetime, across unload and
// reload of the name.
using SoundId = uint32_t;
inline constexpr SoundId kNoSound = 0xFFFFFFFFu;

// Z-DON: everything the mixer callback touches is reached through atomics: a slot per sound name
// (playback state), immutable PCM published by pointer, and retired PCM freed only after the
// callback has moved on. The callback takes no lock and does no I/O.
struct SoundSlot {
    std::atomic<sound*> pcm{nullptr}; // the stored sound (PCM part), null when not loaded
    std::atomic<bool> is_playing{false};
    std::atomic<unsigned int> current_frame{0};
    std::atomic<float> frame_frac{0.0f};
    std::atomic<bool> loop{false};
    std::atomic<float> volume{1.0f};
    std::atomic<uint8_t> category{0}; // a VolumePreset: its live volume replaces `volume` (NONE: volume)
    std::atomic<float> playback_gain{1.0f};
    std::atomic<float> pan{0.5f};
    std::atomic<float> pitch{1.0f};
    std::atomic<uint64_t> play_count{0};
    bool in_active = false; // mixer thread only: listed in the active voices
    // Z-DON polyphony (make_polyphonic): the slot of the name keeps the parameters and plays through a
    // ring of `voices` extra slots (ids voice_base...), each a voice reading the name's PCM.
    std::atomic<SoundId> owner{kNoSound}; // a ring voice: the slot whose PCM it plays
    std::atomic<uint32_t> voices{0};      // the name's slot: ring size (0 = not polyphonic)
    SoundId voice_base = kNoSound;
    std::atomic<uint32_t> next_voice{0}; // the ring's next voice = the oldest one
};

// A music stream. File-backed streams are decoded on the engine's stream decoder thread into a
// single-producer/single-consumer ring; the callback only copies out of it. Seek and stop are
// commands (cmd_gen) the decoder carries out. PCM-backed streams (fully decoded) are read in place.
struct music {
    // fixed at load
    SF_INFO file_info{};
    std::string file_path;
    float normalization_gain = 1.0f;
    bool loop = false;
    float* pcm_data = nullptr; // fully decoded PCM at the device rate (nus3bank preview, FFmpeg fallback)
    sf_count_t pcm_total_frames = 0;

    // shared with the main thread
    std::atomic<bool> is_playing{false};
    std::atomic<unsigned long long> current_frame{0};
    std::atomic<float> volume{1.0f};
    std::atomic<uint8_t> category{0}; // as SoundSlot::category
    std::atomic<float> pan{0.5f};

    // commands, main -> mixer/decoder: a seek target, published by bumping cmd_gen
    std::atomic<long long> cmd_source_frame{0};       // file position to seek to (source rate)
    std::atomic<unsigned long long> cmd_out_frame{0}; // current_frame once the seek is in effect
    std::atomic<uint32_t> cmd_gen{0};

    // decoder only (file-backed)
    SNDFILE* file_handle = nullptr;
    SRC_STATE* resampler = nullptr;
    float* stream_buffer = nullptr;   // source frames read from the file
    float* resample_buffer = nullptr; // resampled frames
    unsigned int buffer_size = 0;     // source frames per read
    uint32_t decoder_gen = 0;
    bool decoder_eof = false;
    std::shared_ptr<std::vector<uint8_t>> memory_buffer;
    VirtualFile vio_cursor;

    // the ring, decoder -> mixer: frames at the device rate, file_info.channels interleaved
    std::unique_ptr<float[]> ring;
    unsigned long long ring_frames = 0;
    std::atomic<unsigned long long> write_pos{0}; // decoder
    std::atomic<unsigned long long> read_pos{0};  // mixer
    std::atomic<unsigned long long> flush_pos{0}; // frames before this belong to an earlier seek
    std::atomic<uint32_t> ring_gen{0};            // the command the data after flush_pos answers
    std::atomic<bool> eof{false};                 // nothing follows write_pos (for ring_gen)

    // mixer only
    uint32_t mixer_gen = 0;

    music() = default;
    music(const music&) = delete;
    music& operator=(const music&) = delete;
    ~music();
};

// Enumerates output device names for the given backend (device_type).
// Returns an empty list on platforms without device enumeration support.
std::vector<std::string> enumerate_audio_devices(int device_type);

class AudioEngine {
public:
    fs::path sounds_path;

    AudioEngine();
    ~AudioEngine();

    bool init_audio_device(const fs::path& sounds_path, const AudioConfig& audio_config,
                           const VolumeConfig& volume_presets);
    void close_audio_device();
    bool is_audio_device_ready() const;
    // Z-DON: the category volumes; sounds and streams already playing follow at the next callback
    void set_volume_presets(const VolumeConfig& presets);

    void queue_song_loudness(const fs::path& file_path, bool priority = false);

    void load_screen_sounds(const std::string& screen_name);

    std::string load_sound(const fs::path& file_path, const std::string& name);
    // Z-DON: load_sound whose PCM stays resident across unloads (screen changes; still stopped and
    // gone from has_sound until loaded again): the next call with the same name and file reinstalls it
    // without decoding, another file replaces it. ZDON_RESIDENT_SOUND_MB caps the set (default 64,
    // 0 = off); closing the device (skin change or reload) drops it. *reused: no decode was needed.
    std::string load_resident_sound(const fs::path& file_path, const std::string& name, bool* reused = nullptr);
    // sounds longer than this decode their tail in the background (Z-DON)
    static constexpr int kProgressiveMinSeconds = 8;
    static constexpr int kProgressiveHeadSeconds = 2;
    void unload_sound(const std::string& name);
    void unload_all_sounds();
    void store_sound(const std::string& name, const sound& snd);
    // the handle of name (created if new; it need not be loaded yet)
    SoundId sound_id(const std::string& name);
    void play_sound(const std::string& name, VolumePreset volume_preset = VolumePreset::NONE, float gain = 1.0f);
    void play_sound(SoundId id, VolumePreset volume_preset = VolumePreset::NONE, float gain = 1.0f);
    // Z-DON: successive plays of id overlap instead of restarting it, up to ZDON_HIT_POLYPHONY voices
    // (default 6, 1 = restart as before), the oldest one taken over by a new play. Idempotent.
    void make_polyphonic(SoundId id);
    bool has_sound(const std::string& name);
    void stop_sound(const std::string& name);
    void set_sound_loop(const std::string& name, bool loop);
    bool is_sound_playing(const std::string& name);
    // times play_sound(name) has been called (a skin can count hits even while the sound
    // is still playing from the previous one)
    uint64_t sound_play_count(const std::string& name);
    void set_sound_volume(const std::string& name, float volume);
    void set_sound_pan(const std::string& name, float pan);
    void set_sound_pitch(const std::string& name, float pitch);
    float get_sound_time_played(const std::string& name) const;
    float get_sound_time_length(const std::string& name) const;
    void seek_sound(const std::string& name, float position);

    struct PreparedPCM {
        std::unique_ptr<float[]> data; // interleaved, at the device rate
        unsigned int frames = 0;
        unsigned int rate = 0;
        int channels = 0;
        int preview_ms = 0; // the bank's own preview point
        std::string source_path;
    };
    bool prepare_nus3bank_pcm(const fs::path& file_path, PreparedPCM& out, bool quick_resample = false);
    std::string load_music_stream_prepared(PreparedPCM&& pcm, const std::string& name);

    std::string load_music_stream(const fs::path& file_path, const std::string& name);
#ifndef __EMSCRIPTEN__
    std::string load_music_stream_memory(const av::AVAudioStream& audio_stream, const std::string& name);
#endif
    void play_music_stream(const std::string& name, VolumePreset volume_preset = VolumePreset::NONE);
    float get_music_time_length(const std::string& name) const;
    float get_music_time_played(const std::string& name) const;
    void set_music_volume(const std::string& name, float volume);
    bool is_music_stream_valid(const std::string& name) const;
    bool is_music_stream_playing(const std::string& name) const;
    void stop_music_stream(const std::string& name);
    void unload_music_stream(const std::string& name);
    void unload_all_music();
#ifdef YATAIDON_PLATFORM_IOS
    void suspend_ios_audio(bool suspended);
#endif
    void seek_music_stream(const std::string& name, float position);

    // Z-DON dev hook YATAIDON_AUDIO_SELFTEST (audio_selftest.cpp)
    void run_selftest(const std::string& spec);

private:
    void dev_pause_device(bool pause);
    void dev_wait_streams();                        // until every stream's decoder has caught up
    void dev_wait_decoded(const std::string& name); // until a progressive sound is fully decoded

    std::unique_ptr<song_loudness::Scanner> loudness_scanner;

    std::string load_sound_progressive(SNDFILE* file, const SF_INFO& info, const fs::path& file_path,
                                       const std::string& name);
    // true when the decoder took over freeing snd.data
    static bool stop_decode_job(sound& snd);
    // stops the decode job and frees the buffers a stored sound owns
    static void free_sound(sound& snd);
    void publish_sound(const std::string& name, SoundId id, sound* pcm);
    void retire_sound(sound* old); // frees it once no callback can be using it; null is a no-op

    // resident sounds (load_resident_sound): name -> PCM no unload frees
    struct ResidentSound {
        std::string path;
        sound* pcm = nullptr;
        size_t bytes = 0;
    };
    // also held across a slot's PCM exchange, so ownership stays clear. Lock order: resident_lock, then
    // names_lock; never retire while holding it (reclaim takes names_lock unique)
    std::mutex resident_lock;
    std::unordered_map<std::string, ResidentSound> resident;
    size_t resident_bytes = 0;
    bool is_resident_locked(const std::string& name, const sound* pcm) const;
    void drop_resident_sounds();
    void install_music(const std::string& name, std::shared_ptr<music> mus);
    double target_sample_rate;
    unsigned long buffer_size;
    std::string device_name; // Output device to open (empty = system default)
    std::vector<int> channel_offsets{0};
    unsigned int rt_total_channels = 2;
    VolumeConfig volume_presets;
    std::atomic<float> category_volume[(int)VolumePreset::ATTRACT_MODE + 1]{}; // live; the mixer reads them
    float base_volume(uint8_t category, const std::atomic<float>& own) const {
        return category ? category_volume[category].load(std::memory_order_relaxed)
                        : own.load(std::memory_order_relaxed);
    }
    bool is_ready;
    std::atomic<float> master_volume;

    SDL_AudioStream* sdl_stream = nullptr;
    bool sdl_audio_subsystem_initialized = false;
    std::vector<float> sdl_scratch_buffer;

#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
    RtAudio* rt_audio = nullptr; // ALSA/JACK/PulseAudio/OSS/CoreAudio/DirectSound/ASIO/WASAPI
#endif

    // ---- sounds: name -> slot (main/loader threads, under names_lock; never the callback) ----
    static constexpr size_t kSlotChunk = 256;
    static constexpr size_t kMaxSlotChunks = 64;
    mutable std::shared_mutex names_lock;
    std::unordered_map<std::string, SoundId> sound_ids;
    std::atomic<SoundSlot*> slot_chunks[kMaxSlotChunks]{};
    std::atomic<uint32_t> slot_count{0};
    SoundSlot* slot_at(SoundId id) const {
        return &slot_chunks[id / kSlotChunk].load(std::memory_order_acquire)[id % kSlotChunk];
    }
    SoundId alloc_slot_locked(); // under names_lock (unique); kNoSound when full
    // the voice of a polyphonic slot (voices > 0) that the last play started
    SoundSlot& latest_voice(SoundSlot& s) const {
        const uint32_t n = s.voices.load(std::memory_order_acquire);
        return n ? *slot_at(s.voice_base + (s.next_voice.load(std::memory_order_relaxed) + n - 1) % n) : s;
    }
    template<class Fn>
    void for_each_voice(SoundSlot& s, Fn&& fn) const { // the slot itself and its ring's voices
        fn(s);
        const uint32_t n = s.voices.load(std::memory_order_acquire);
        for (uint32_t k = 0; k < n; k++) {
            fn(*slot_at(s.voice_base + k));
        }
    }
    SoundId find_sound(const std::string& name) const; // kNoSound if the name was never used
    template<class Fn>
    bool with_sound(const std::string& name, Fn&& fn) const; // fn(slot, pcm) if loaded

    // play_sound -> mixer: slots to add to the active voices (bounded MPMC queue)
    static constexpr size_t kActivateCap = 1024;
    struct ActivateCell {
        std::atomic<size_t> seq;
        SoundId id;
    };
    std::unique_ptr<ActivateCell[]> activate_cells;
    std::atomic<size_t> activate_head{0}, activate_tail{0};
    std::atomic<bool> activate_rescan{false}; // the queue overflowed: the mixer scans every slot once
    void activate(SoundId id);
    std::vector<SoundId> active_voices; // mixer only; capacity reserved up front

    // ---- music streams ----
    mutable std::mutex music_lock; // main/loader threads; never the callback
    std::unordered_map<std::string, std::shared_ptr<music>> music_streams;
    std::shared_ptr<music> find_music(const std::string& name) const;
    struct StreamList {
        std::vector<music*> streams;
    };
    std::atomic<StreamList*> mixer_streams{nullptr}; // what the callback mixes
    StreamList* publish_streams();                   // under music_lock; returns the old list to retire

    // the stream decoder thread
    std::thread decoder_thread;
    std::mutex decoder_mutex;
    std::condition_variable decoder_cv;
    bool decoder_running = false;
    bool decoder_kick = false;
    std::vector<std::shared_ptr<music>> decoder_streams; // under decoder_mutex
    void start_decoder();
    void stop_decoder();
    void kick_decoder();
    void decoder_main();
    static bool decode_into_ring(music& mus, double target_rate); // one read; false when nothing was written

    // ---- deferred reclamation: freed once no callback can still be using it ----
    std::atomic<uint64_t> mix_epoch{0}; // odd while a callback runs
    std::mutex retire_lock;
    struct Retired {
        uint64_t safe_epoch;
        std::function<void()> free;
    };
    std::vector<Retired> retired;
    void retire(std::function<void()> free);
    void reclaim(bool force = false);

    std::string path_to_string(const fs::path& path) const;
    float loaded_sound_gain(const fs::path& file_path, const std::string& name);
    float loaded_song_gain(const fs::path& file_path);

#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
    bool init_rtaudio_device(RtAudio::Api api, const char* label);
#endif
    bool init_sdl3_device();

    static void mix(float* out, unsigned int framesPerBuffer, AudioEngine* engine);
    void mix_sounds(float* out, unsigned int frames);
    void mix_streams(float* out, unsigned int frames);
    static void sdl_audio_callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
#if !defined(__ANDROID__) && !defined(YATAIDON_PLATFORM_IOS) && !defined(__EMSCRIPTEN__)
    static int rt_audio_callback(void* outputBuffer, void* inputBuffer, unsigned int framesPerBuffer, double streamTime,
                                 unsigned int status, void* userData);
#endif
};

extern AudioEngine audio;
