/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer: Audio module

A small fixed-voice mixer on one AAudio stream. AAudio is part of the NDK
and needs no JNI, which matters here because the app is a NativeActivity
with no Java layer to set anything up.

Sound effects (10 WAVs, all 44.1 kHz 16-bit stereo already) are decoded up
front. Music is Ogg Vorbis and far too large to hold decoded -- 14 MB of
Vorbis would be hundreds of MB of PCM -- so each Music object runs a
decoder thread filling a ring buffer, and the audio callback only ever
drains it. Nothing does file I/O on the audio thread.
---------------------------------------------------------------------*/

#include "SFML/Audio.hpp"
#include "platform.h"

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ETRAudio", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "ETRAudio", __VA_ARGS__)

namespace {

const int kSampleRate = 44100;
const int kChannels   = 2;
const int kMaxVoices  = 24;

struct Voice {
	const sf::SoundBuffer* buffer = nullptr;
	std::size_t position = 0;
	bool active = false;
	bool loop = false;
	float volume = 1.f;
};

/** Single-producer/single-consumer ring of interleaved S16 frames, shared
 *  between a Music decoder thread and the audio callback. */
class Ring {
public:
	void reset(std::size_t frames) {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_data.assign(frames * kChannels, 0);
		m_read = m_write = m_fill = 0;
	}

	std::size_t writable() const {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_data.empty()) return 0;
		return (m_data.size() - m_fill) / kChannels;
	}

	void write(const sf::Int16* src, std::size_t frames) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_data.empty()) return;
		for (std::size_t i = 0; i < frames * kChannels; i++) {
			if (m_fill >= m_data.size()) break;
			m_data[m_write] = src[i];
			m_write = (m_write + 1) % m_data.size();
			m_fill++;
		}
	}

	std::size_t read(sf::Int16* dst, std::size_t frames) {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_data.empty()) return 0;
		std::size_t done = 0;
		for (; done < frames * kChannels; done++) {
			if (m_fill == 0) break;
			dst[done] = m_data[m_read];
			m_read = (m_read + 1) % m_data.size();
			m_fill--;
		}
		return done / kChannels;
	}

private:
	mutable std::mutex m_mutex;
	std::vector<sf::Int16> m_data;
	std::size_t m_read = 0, m_write = 0, m_fill = 0;
};

struct MusicStream {
	Ring ring;
	std::atomic<bool> playing{false};
	std::atomic<bool> quit{false};
	std::atomic<bool> loop{false};
	std::atomic<float> volume{1.f};
	std::thread thread;
	stb_vorbis* vorbis = nullptr;
	std::mutex decode_mutex;
};

struct Mixer {
	AAudioStream* stream = nullptr;
	std::mutex mutex;
	Voice voices[kMaxVoices];
	std::vector<MusicStream*> streams;
	std::vector<sf::Int16> scratch;
	std::vector<int> accumulator;
	bool started = false;
};

Mixer g_mixer;

void mixInto(sf::Int16* out, int frames) {
	// Accumulate at 32 bits so several simultaneous effects cannot wrap.
	g_mixer.accumulator.assign(static_cast<std::size_t>(frames) * kChannels, 0);
	std::vector<int>& acc = g_mixer.accumulator;

	{
		std::lock_guard<std::mutex> lock(g_mixer.mutex);

		for (Voice& v : g_mixer.voices) {
			if (!v.active || !v.buffer) continue;

			const sf::Int16* samples = v.buffer->getSamples();
			const std::size_t total = v.buffer->getSampleCount();
			const unsigned int src_channels = v.buffer->getChannelCount();
			if (total == 0 || src_channels == 0) { v.active = false; continue; }

			for (int f = 0; f < frames; f++) {
				if (v.position >= total) {
					if (!v.loop) { v.active = false; break; }
					v.position = 0;
				}
				// Mono effects are duplicated to both ears; stereo passes
				// through untouched.
				const int l = samples[v.position];
				const int r = (src_channels >= 2 && v.position + 1 < total)
				            ? samples[v.position + 1] : l;

				acc[f * kChannels]     += static_cast<int>(l * v.volume);
				acc[f * kChannels + 1] += static_cast<int>(r * v.volume);
				v.position += src_channels;
			}
		}

		if (g_mixer.scratch.size() <
		    static_cast<std::size_t>(frames) * kChannels)
			g_mixer.scratch.resize(static_cast<std::size_t>(frames) * kChannels);

		for (MusicStream* ms : g_mixer.streams) {
			if (!ms->playing.load()) continue;

			std::fill(g_mixer.scratch.begin(), g_mixer.scratch.end(), 0);
			const std::size_t got = ms->ring.read(g_mixer.scratch.data(), frames);
			if (got == 0) continue;

			const float vol = ms->volume.load();
			for (std::size_t i = 0; i < got * kChannels; i++)
				acc[i] += static_cast<int>(g_mixer.scratch[i] * vol);
		}
	}

	for (std::size_t i = 0; i < acc.size(); i++)
		out[i] = static_cast<sf::Int16>(std::max(-32768, std::min(32767, acc[i])));
}

aaudio_data_callback_result_t audioCallback(AAudioStream*, void*,
                                            void* audioData,
                                            int32_t numFrames) {
	mixInto(static_cast<sf::Int16*>(audioData), numFrames);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool ensureDevice() {
	if (g_mixer.started) return true;

	AAudioStreamBuilder* builder = nullptr;
	if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
		LOGE("AAudio_createStreamBuilder failed");
		return false;
	}

	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setChannelCount(builder, kChannels);
	AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
	AAudioStreamBuilder_setPerformanceMode(builder,
	                                       AAUDIO_PERFORMANCE_MODE_NONE);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setDataCallback(builder, audioCallback, nullptr);

	const aaudio_result_t r =
	    AAudioStreamBuilder_openStream(builder, &g_mixer.stream);
	AAudioStreamBuilder_delete(builder);

	if (r != AAUDIO_OK || !g_mixer.stream) {
		LOGE("AAudio openStream failed: %s", AAudio_convertResultToText(r));
		return false;
	}

	AAudioStream_requestStart(g_mixer.stream);
	g_mixer.started = true;
	LOGI("AAudio stream open at %d Hz", kSampleRate);
	return true;
}

void musicThread(MusicStream* ms) {
	std::vector<sf::Int16> chunk(4096 * kChannels);

	while (!ms->quit.load()) {
		if (!ms->playing.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		const std::size_t space = ms->ring.writable();
		if (space < 1024) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}

		std::lock_guard<std::mutex> lock(ms->decode_mutex);
		if (!ms->vorbis) continue;

		const int want = static_cast<int>(std::min<std::size_t>(space, 4096));
		const int got = stb_vorbis_get_samples_short_interleaved(
		    ms->vorbis, kChannels, chunk.data(), want * kChannels);

		if (got > 0) {
			ms->ring.write(chunk.data(), static_cast<std::size_t>(got));
		} else if (ms->loop.load()) {
			stb_vorbis_seek_start(ms->vorbis);
		} else {
			ms->playing.store(false);
		}
	}
}

/** Minimal RIFF/WAVE reader. Every sound effect the game ships is already
 *  44.1 kHz 16-bit stereo, so anything else is reported rather than
 *  silently resampled. */
bool loadWav(const std::string& filename, std::vector<sf::Int16>& out,
             unsigned int& channels, unsigned int& rate) {
	FILE* f = std::fopen(filename.c_str(), "rb");
	if (!f) return false;

	char riff[12];
	if (std::fread(riff, 1, 12, f) != 12 ||
	    std::memcmp(riff, "RIFF", 4) != 0 ||
	    std::memcmp(riff + 8, "WAVE", 4) != 0) {
		std::fclose(f);
		return false;
	}

	unsigned short fmt = 0, ch = 0, bits = 0;
	unsigned int sample_rate = 0;
	bool have_fmt = false;

	for (;;) {
		char id[4];
		unsigned int size = 0;
		if (std::fread(id, 1, 4, f) != 4) break;
		if (std::fread(&size, 4, 1, f) != 1) break;

		if (std::memcmp(id, "fmt ", 4) == 0) {
			unsigned char buf[16];
			if (size < 16 || std::fread(buf, 1, 16, f) != 16) break;
			fmt   = static_cast<unsigned short>(buf[0] | (buf[1] << 8));
			ch    = static_cast<unsigned short>(buf[2] | (buf[3] << 8));
			sample_rate = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
			bits  = static_cast<unsigned short>(buf[14] | (buf[15] << 8));
			have_fmt = true;
			if (size > 16) std::fseek(f, size - 16, SEEK_CUR);
		} else if (std::memcmp(id, "data", 4) == 0) {
			if (!have_fmt || fmt != 1 || bits != 16) {
				LOGE("'%s' is not 16-bit PCM (format %u, %u bits)",
				     filename.c_str(), fmt, bits);
				std::fclose(f);
				return false;
			}
			out.resize(size / 2);
			const std::size_t got = std::fread(out.data(), 2, out.size(), f);
			out.resize(got);
			channels = ch;
			rate = sample_rate;
			std::fclose(f);
			return !out.empty();
		} else {
			std::fseek(f, size + (size & 1), SEEK_CUR);
		}
	}

	std::fclose(f);
	return false;
}

}  // namespace

namespace sf {

// --------------------------------------------------------------------
//	SoundBuffer
// --------------------------------------------------------------------

SoundBuffer::SoundBuffer() : m_channels(0), m_sample_rate(0) {}
SoundBuffer::~SoundBuffer() {}

bool SoundBuffer::loadFromFile(const std::string& filename) {
	if (loadWav(filename, m_samples, m_channels, m_sample_rate)) {
		if (m_sample_rate != kSampleRate) {
			// Nothing the game ships hits this, but a mismatch would play
			// back at the wrong pitch, so say so rather than hide it.
			LOGE("'%s' is %u Hz, expected %d -- it will play back detuned",
			     filename.c_str(), m_sample_rate, kSampleRate);
		}
		return true;
	}

	// Not a WAV -- try Vorbis, so either format works for effects.
	int channels = 0, rate = 0;
	short* decoded = nullptr;
	const int frames = stb_vorbis_decode_filename(filename.c_str(), &channels,
	                                              &rate, &decoded);
	if (frames <= 0 || !decoded) {
		LOGE("failed to load sound '%s'", filename.c_str());
		return false;
	}
	m_samples.assign(decoded,
	                 decoded + static_cast<std::size_t>(frames) * channels);
	m_channels = static_cast<unsigned int>(channels);
	m_sample_rate = static_cast<unsigned int>(rate);
	std::free(decoded);
	return true;
}

// --------------------------------------------------------------------
//	Sound
// --------------------------------------------------------------------

Sound::Sound() : m_buffer(nullptr), m_voice(-1), m_loop(false), m_volume(1.f) {}

Sound::~Sound() { stop(); }

void Sound::setBuffer(const SoundBuffer& buffer) { m_buffer = &buffer; }

void Sound::play() {
	if (!m_buffer || !ensureDevice()) return;

	std::lock_guard<std::mutex> lock(g_mixer.mutex);

	// Reuse this Sound's own voice if it is still running, so repeated
	// play() calls restart it rather than stacking copies.
	int slot = -1;
	if (m_voice >= 0 && g_mixer.voices[m_voice].buffer == m_buffer) {
		slot = m_voice;
	} else {
		for (int i = 0; i < kMaxVoices; i++) {
			if (!g_mixer.voices[i].active) { slot = i; break; }
		}
	}
	if (slot < 0) return;  // all voices busy: drop the effect

	Voice& v = g_mixer.voices[slot];
	v.buffer = m_buffer;
	v.position = 0;
	v.active = true;
	v.loop = m_loop;
	v.volume = m_volume;
	m_voice = slot;
}

void Sound::stop() {
	if (m_voice < 0) return;
	std::lock_guard<std::mutex> lock(g_mixer.mutex);
	g_mixer.voices[m_voice].active = false;
	m_voice = -1;
}

void Sound::pause() { stop(); }

void Sound::setLoop(bool loop) {
	m_loop = loop;
	if (m_voice >= 0) {
		std::lock_guard<std::mutex> lock(g_mixer.mutex);
		g_mixer.voices[m_voice].loop = loop;
	}
}

void Sound::setVolume(float volume) {
	m_volume = volume / 100.f;
	if (m_voice >= 0) {
		std::lock_guard<std::mutex> lock(g_mixer.mutex);
		g_mixer.voices[m_voice].volume = m_volume;
	}
}

Sound::Status Sound::getStatus() const {
	if (m_voice < 0) return Stopped;
	std::lock_guard<std::mutex> lock(g_mixer.mutex);
	return g_mixer.voices[m_voice].active ? Playing : Stopped;
}

// --------------------------------------------------------------------
//	Music
// --------------------------------------------------------------------

struct Music::Impl {
	MusicStream stream;
};

Music::Music() : m_impl(new Impl) {}

Music::~Music() {
	m_impl->stream.playing.store(false);
	m_impl->stream.quit.store(true);
	if (m_impl->stream.thread.joinable()) m_impl->stream.thread.join();

	{
		std::lock_guard<std::mutex> lock(g_mixer.mutex);
		auto& v = g_mixer.streams;
		v.erase(std::remove(v.begin(), v.end(), &m_impl->stream), v.end());
	}
	if (m_impl->stream.vorbis) stb_vorbis_close(m_impl->stream.vorbis);
}

bool Music::openFromFile(const std::string& filename) {
	if (!ensureDevice()) return false;

	int error = 0;
	stb_vorbis* v = stb_vorbis_open_filename(filename.c_str(), &error, nullptr);
	if (!v) {
		LOGE("failed to open music '%s' (error %d)", filename.c_str(), error);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_impl->stream.decode_mutex);
		if (m_impl->stream.vorbis) stb_vorbis_close(m_impl->stream.vorbis);
		m_impl->stream.vorbis = v;
	}
	// Roughly half a second of slack, ample to ride out a scheduling hiccup
	// on the decoder thread.
	m_impl->stream.ring.reset(kSampleRate / 2);

	if (!m_impl->stream.thread.joinable()) {
		{
			std::lock_guard<std::mutex> lock(g_mixer.mutex);
			g_mixer.streams.push_back(&m_impl->stream);
		}
		m_impl->stream.thread = std::thread(musicThread, &m_impl->stream);
	}
	return true;
}

void Music::play() {
	if (!m_impl->stream.vorbis) return;
	m_impl->stream.playing.store(true);
}

void Music::stop() {
	m_impl->stream.playing.store(false);
	{
		std::lock_guard<std::mutex> lock(m_impl->stream.decode_mutex);
		if (m_impl->stream.vorbis) stb_vorbis_seek_start(m_impl->stream.vorbis);
	}
	m_impl->stream.ring.reset(kSampleRate / 2);
}

void Music::pause() { m_impl->stream.playing.store(false); }

void Music::setLoop(bool loop) { m_impl->stream.loop.store(loop); }
bool Music::getLoop() const { return m_impl->stream.loop.load(); }

void Music::setVolume(float volume) {
	m_impl->stream.volume.store(volume / 100.f);
}

float Music::getVolume() const { return m_impl->stream.volume.load() * 100.f; }

Music::Status Music::getStatus() const {
	return m_impl->stream.playing.load() ? Playing : Stopped;
}

}  // namespace sf
