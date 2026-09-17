/* --------------------------------------------------------------------
EXTREME TUXRACER -- SFML compatibility layer for Android

Audio module. The game only needs one-shot sound effects (10 WAVs) and
streamed music (10 Ogg Vorbis files), which is a small enough surface to
sit directly on an AAudio stream: WAVs are decoded up front by a small
RIFF reader, music is decoded incrementally with stb_vorbis on its own
thread, and the audio callback only mixes.
---------------------------------------------------------------------*/

#ifndef ETR_SFML_AUDIO_HPP
#define ETR_SFML_AUDIO_HPP

#include "System.hpp"

#include <memory>
#include <string>
#include <vector>

namespace sf {

class SoundSource {
public:
	enum Status { Stopped, Paused, Playing };
};

/** Fully decoded PCM, shared by every Sound that plays it. */
class SoundBuffer {
public:
	SoundBuffer();
	~SoundBuffer();

	bool loadFromFile(const std::string& filename);

	const Int16* getSamples() const { return m_samples.data(); }
	std::size_t getSampleCount() const { return m_samples.size(); }
	unsigned int getChannelCount() const { return m_channels; }
	unsigned int getSampleRate() const { return m_sample_rate; }

private:
	friend class Sound;
	std::vector<Int16> m_samples;
	unsigned int m_channels;
	unsigned int m_sample_rate;
};

class Sound : public SoundSource {
public:
	Sound();
	~Sound();

	void setBuffer(const SoundBuffer& buffer);
	void play();
	void stop();
	void pause();

	void setLoop(bool loop);
	bool getLoop() const { return m_loop; }

	/** Volume is on SFML's 0..100 scale. */
	void setVolume(float volume);
	float getVolume() const { return m_volume; }

	Status getStatus() const;

private:
	const SoundBuffer* m_buffer;
	int m_voice;
	bool m_loop;
	float m_volume;
};

/** Streamed audio. Decoded a chunk at a time so the 14 MB of music does
 *  not have to sit in memory. */
class Music : public SoundSource {
public:
	Music();
	~Music();

	bool openFromFile(const std::string& filename);

	void play();
	void stop();
	void pause();

	void setLoop(bool loop);
	bool getLoop() const;

	void setVolume(float volume);
	float getVolume() const;

	Status getStatus() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

}  // namespace sf

#endif  // ETR_SFML_AUDIO_HPP
