#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <Aulib/Stream.h>

#include "engine/sound_defs.hpp"
#include "utils/stdcompat/shared_ptr_array.hpp"

namespace devilution {

struct OpenAlBuffer;

class SoundSample final {
public:
	SoundSample() = default;
	SoundSample(SoundSample &&) noexcept = default;
	SoundSample &operator=(SoundSample &&) noexcept = default;
	~SoundSample();

	[[nodiscard]] bool IsLoaded() const
	{
		return al_source_ != 0 || stream_ != nullptr;
	}

	void Release();
	bool IsPlaying();

	// Returns 0 on success.
	int SetChunkStream(std::string filePath, bool isMp3, bool logErrors = true);

	void SetFinishCallback(Aulib::Stream::Callback &&callback)
	{
		if (stream_ != nullptr)
			stream_->setFinishCallback(std::forward<Aulib::Stream::Callback>(callback));
	}

	/**
	 * @brief Sets the sample's WAV, FLAC, or Ogg/Vorbis data.
	 * @param fileData Buffer containing the data
	 * @param dwBytes Length of buffer
	 * @param isMp3 Whether the data is an MP3
	 * @return 0 on success, -1 otherwise
	 */
	int SetChunk(ArraySharedPtr<std::uint8_t> fileData, std::size_t dwBytes, bool isMp3);

	[[nodiscard]] bool IsStreaming() const
	{
		return al_buffer_ == nullptr && file_data_ == nullptr;
	}

	[[nodiscard]] bool MatchesData(const SoundSample &other) const
	{
		if (al_buffer_ != nullptr && other.al_buffer_ != nullptr)
			return al_buffer_.get() == other.al_buffer_.get();
		return file_data_ != nullptr && file_data_.get() == other.file_data_.get();
	}

	int DuplicateFrom(const SoundSample &other);

	/**
	 * @brief Start playing the sound for a given number of iterations (0 means loop).
	 */
	bool Play(int numIterations = 1);

	/**
	 * @brief Start playing the sound with the given sound and user volume, and a stereo position.
	 */
	bool PlayWithVolumeAndPan(int logSoundVolume, int logUserVolume, int logPan)
	{
		return PlayWith3D(logSoundVolume, logUserVolume, logPan, 0, 0, false, false, false);
	}

	/**
	 * @brief Start playing the sound with OpenAL 3D spatialization, HRTF, EFX reverb, and wall occlusion.
	 */
	bool PlayWith3D(int logSoundVolume, int logUserVolume, int logPan, int deltaX, int deltaY, bool isPositional, bool applyReverb, bool isOccluded, bool loop = false);

	/**
	 * @brief Stop playing the sound
	 */
	void Stop();

	void SetVolume(int logVolume, int logMin, int logMax);
	void SetStereoPosition(int logPan);

	void Mute();
	void Unmute();

	/**
	 * @return Audio duration in ms
	 */
	int GetLength() const;

private:
	// OpenAL Soft fields for low-latency 3D/HRTF/EFX playback:
	std::shared_ptr<OpenAlBuffer> al_buffer_;
	std::uint32_t al_source_ = 0;
	float saved_gain_ = 1.0F;
	bool muted_ = false;

	// Non-streaming fallback audio fields:
	ArraySharedPtr<std::uint8_t> file_data_;
	std::size_t file_data_size_ = 0;

	// Set for streaming audio to allow for duplicating it:
	std::string file_path_;

	bool isMp3_ = false;

	std::unique_ptr<Aulib::Stream> stream_;
};

} // namespace devilution
