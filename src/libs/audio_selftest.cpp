// Z-DON dev hook YATAIDON_AUDIO_SELFTEST (see audio_dev.h): mixes a fixed script offline, so two builds'
// mixers can be compared sample by sample.
#include <cstdio>
#include <fstream>
#include <vector>

#include "audio.h"
#include "audio_dev.h"
#include "spdlog/spdlog.h"

void AudioEngine::run_selftest(const std::string& spec) {
    std::vector<std::string> f;
    size_t st = 0;
    for (size_t i; (i = spec.find(';', st)) != std::string::npos; st = i + 1) {
        f.push_back(spec.substr(st, i - st));
    }
    f.push_back(spec.substr(st));
    if (f.size() < 3) {
        spdlog::error("YATAIDON_AUDIO_SELFTEST: <out.wav>;<long sound>;<stream file>");
        return;
    }
    const std::string out_path = f[0];
    const fs::path long_file = fs::u8path(f[1]);
    const fs::path stream_file = fs::u8path(f[2]);

    dev_pause_device(true);
    std::vector<float> out;
    std::ofstream log(out_path + ".txt");
    const double rate = target_sample_rate;

    auto mix_for = [&](double seconds, unsigned int block) {
        size_t frames = (size_t)(seconds * rate);
        std::vector<float> buf(block * 2);
        while (frames > 0) {
            const unsigned int n = (unsigned int)std::min<size_t>(frames, block);
            dev_wait_streams();
            mix(buf.data(), n, this);
            out.insert(out.end(), buf.begin(), buf.begin() + n * 2);
            frames -= n;
        }
    };
    auto state = [&](const char* step) {
        log << step << " frame=" << out.size() / 2;
        for (const char* s : {"st_don", "st_kat", "st_long", "st_missing"}) {
            const bool has = has_sound(s);
            log << " | " << s << " has=" << has << " count=" << sound_play_count(s);
            if (has) {
                log << " playing=" << is_sound_playing(s) << " t=" << get_sound_time_played(s)
                    << " len=" << get_sound_time_length(s);
            }
        }
        if (is_music_stream_valid("st_stream")) {
            log << " | st_stream playing=" << is_music_stream_playing("st_stream")
                << " t=" << get_music_time_played("st_stream") << " len=" << get_music_time_length("st_stream");
        }
        log << "\n";
    };

    // master volume and clamp apply as in the game; the test sounds stay well below full scale
    const float saved_master = master_volume.load();
    master_volume.store(1.0f);

    load_sound(sounds_path / "don.wav", "st_don");
    load_sound(sounds_path / "ka.wav", "st_kat");
    load_sound(long_file, "st_long");
    dev_wait_decoded("st_long");
    state("loaded");

    play_sound("st_don", VolumePreset::HITSOUND);
    mix_for(0.3, 231);
    state("don");
    play_sound("st_kat", VolumePreset::SOUND, 0.7f);
    set_sound_pan("st_kat", 0.2f);
    mix_for(0.1, 231);
    play_sound("st_don", VolumePreset::HITSOUND); // restart while playing
    mix_for(0.2, 231);
    state("kat+don");
    set_sound_pitch("st_kat", 1.37f);
    play_sound("st_kat", VolumePreset::HITSOUND);
    mix_for(0.4, 64);
    state("pitch");
    set_sound_loop("st_don", true);
    play_sound("st_don", VolumePreset::SOUND);
    mix_for(1.0, 1000);
    state("loop");
    stop_sound("st_don");
    set_sound_loop("st_don", false);
    mix_for(0.1, 1000);
    state("stop");
    play_sound("st_long", VolumePreset::MUSIC);
    mix_for(1.0, 4096);
    seek_sound("st_long", 5.0f);
    mix_for(0.5, 4096);
    set_sound_volume("st_long", 0.5f);
    mix_for(0.5, 333);
    state("long");
    stop_sound("st_long");

    load_music_stream(stream_file, "st_stream");
    play_music_stream("st_stream", VolumePreset::MUSIC);
    mix_for(1.0, 231);
    state("stream");
    seek_music_stream("st_stream", 10.0f);
    mix_for(1.0, 231);
    play_sound("st_don", VolumePreset::HITSOUND);
    mix_for(0.3, 512);
    set_music_volume("st_stream", 0.3f);
    mix_for(0.5, 231);
    state("stream seek");
    stop_music_stream("st_stream");
    mix_for(0.1, 231);
    state("stream stop");
    play_music_stream("st_stream", VolumePreset::MUSIC);
    mix_for(0.5, 231);
    state("stream replay");
    seek_music_stream("st_stream", get_music_time_length("st_stream") - 0.5f);
    mix_for(1.5, 1024);
    state("stream end");

    // replacing a sound while it plays, unloading one while it plays, a missing one
    play_sound("st_don", VolumePreset::HITSOUND);
    mix_for(0.05, 231);
    load_sound(sounds_path / "ka.wav", "st_don");
    mix_for(0.1, 231);
    state("replaced");
    play_sound("st_don", VolumePreset::HITSOUND);
    mix_for(0.2, 231);
    play_sound("st_kat", VolumePreset::HITSOUND);
    mix_for(0.05, 231);
    unload_sound("st_kat");
    mix_for(0.1, 231);
    play_sound("st_missing", VolumePreset::HITSOUND);
    state("unloaded");

    unload_sound("st_don");
    unload_sound("st_long");
    unload_music_stream("st_stream");
    master_volume.store(saved_master);

    FILE* wav = std::fopen(out_path.c_str(), "wb");
    if (wav) {
        auto u32 = [wav](uint32_t v) { std::fwrite(&v, 4, 1, wav); };
        auto u16 = [wav](uint16_t v) { std::fwrite(&v, 2, 1, wav); };
        const uint32_t bytes = (uint32_t)(out.size() * sizeof(float));
        std::fwrite("RIFF", 1, 4, wav);
        u32(36 + bytes);
        std::fwrite("WAVEfmt ", 1, 8, wav);
        u32(16);
        u16(3);
        u16(2);
        u32((uint32_t)rate);
        u32((uint32_t)rate * 8);
        u16(8);
        u16(32);
        std::fwrite("data", 1, 4, wav);
        u32(bytes);
        std::fwrite(out.data(), sizeof(float), out.size(), wav);
        std::fclose(wav);
    }
    spdlog::info("YATAIDON_AUDIO_SELFTEST: wrote {} frames to {}", out.size() / 2, out_path);
    dev_pause_device(false);
}
