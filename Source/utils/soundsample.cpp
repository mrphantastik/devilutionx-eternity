#ifndef AL_LIBTYPE_STATIC
#define AL_LIBTYPE_STATIC
#endif
#ifndef AL_ALEXT_PROTOTYPES
#define AL_ALEXT_PROTOTYPES
#endif

#include "utils/soundsample.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include <Aulib/DecoderDrmp3.h>
#include <Aulib/DecoderDrwav.h>
#include <../3rdparty/dr_libs/dr_mp3.h>
#include <../3rdparty/dr_libs/dr_wav.h>
#include <SDL.h>
#ifdef USE_SDL1
#include "utils/sdl2_to_1_2_backports.h"
#else
#include "utils/sdl2_backports.h"
#endif

#include "engine/assets.hpp"
#include "engine/sound.h"
#include "options.h"
#include "utils/aulib.hpp"
#include "utils/log.hpp"
#include "utils/math.h"
#include "utils/stubs.h"

namespace devilution {

struct OpenAlBuffer {
	ALuint monoId = 0;
	ALuint stereoId = 0;
	int durationMs = 0;
	int channels = 1;
	int sampleRate = 22050;

	~OpenAlBuffer()
	{
		if (IsOpenAlActive()) {
			if (monoId != 0)
				alDeleteBuffers(1, &monoId);
			if (stereoId != 0 && stereoId != monoId)
				alDeleteBuffers(1, &stereoId);
		}
	}
};

namespace {

constexpr float LogBase = 10.0F;

/**
 * Scaling factor for attenuating volume.
 * Picked so that a volume change of -10 dB results in half perceived loudness.
 * VolumeScale = -1000 / log(0.5)
 */
constexpr float VolumeScale = 3321.9281F;

/**
 * Min and max volume range, in millibel.
 * -100 dB (muted) to 0 dB (max. loudness).
 */
constexpr float MillibelMin = -10000.F;
constexpr float MillibelMax = 0.F;

/**
 * Stereo separation factor for left/right speaker panning.
 */
constexpr float StereoSeparation = 6000.F;

float PanLogToLinear(int logPan)
{
	if (logPan == 0)
		return 0.0F;

	auto factor = std::pow(LogBase, static_cast<float>(-std::abs(logPan)) / StereoSeparation);

	return std::copysign(1.F - factor, static_cast<float>(logPan));
}

std::unique_ptr<Aulib::Decoder> CreateDecoder(bool isMp3)
{
	if (isMp3)
		return std::make_unique<Aulib::DecoderDrmp3>();
	return std::make_unique<Aulib::DecoderDrwav>();
}

std::unique_ptr<Aulib::Stream> CreateStream(SDL_RWops *handle, bool isMp3)
{
	auto decoder = CreateDecoder(isMp3);
	if (!decoder->open(handle))
		return nullptr;
	auto resampler = CreateAulibResampler(decoder->getRate());
	return std::make_unique<Aulib::Stream>(handle, std::move(decoder), std::move(resampler), /*closeRw=*/true);
}

float VolumeLogToLinear(int logVolume, int logMin, int logMax)
{
	const auto logScaled = math::Remap(static_cast<float>(logMin), static_cast<float>(logMax), MillibelMin, MillibelMax, static_cast<float>(logVolume));
	return std::pow(LogBase, logScaled / VolumeScale);
}

/**
 * @brief Decodes raw WAV or MP3 memory directly via dr_wav / dr_mp3 into native 16-bit PCM frames
 * and creates both a mono buffer (for 3D positional HRTF/surround spatialization) and a stereo
 * buffer (for direct uncolored 2D UI/inventory/player-centered playback that bypasses HRTF).
 */
std::shared_ptr<OpenAlBuffer> DecodeMemoryToOpenAlBuffer(const std::uint8_t *data, std::size_t dwBytes, bool isMp3)
{
	if (data == nullptr || dwBytes == 0)
		return nullptr;

	int channels = 0;
	int sampleRate = 0;
	std::vector<std::int16_t> pcm;

	if (!isMp3) {
		drwav wav;
		if (!drwav_init_memory(&wav, data, dwBytes, nullptr))
			return nullptr;

		channels = static_cast<int>(wav.channels);
		sampleRate = static_cast<int>(wav.sampleRate);
		if ((channels != 1 && channels != 2) || sampleRate <= 0 || wav.totalPCMFrameCount == 0) {
			drwav_uninit(&wav);
			return nullptr;
		}

		const drwav_uint64 totalFrames = wav.totalPCMFrameCount;
		pcm.resize(static_cast<std::size_t>(totalFrames * static_cast<drwav_uint64>(channels)));
		const drwav_uint64 framesRead = drwav_read_pcm_frames_s16(&wav, totalFrames, pcm.data());
		drwav_uninit(&wav);

		if (framesRead == 0)
			return nullptr;
		pcm.resize(static_cast<std::size_t>(framesRead * static_cast<drwav_uint64>(channels)));
	} else {
		drmp3 mp3;
		if (!drmp3_init_memory(&mp3, data, dwBytes, nullptr))
			return nullptr;

		channels = static_cast<int>(mp3.channels);
		sampleRate = static_cast<int>(mp3.sampleRate);
		if ((channels != 1 && channels != 2) || sampleRate <= 0) {
			drmp3_uninit(&mp3);
			return nullptr;
		}

		pcm.reserve(32768);
		std::array<std::int16_t, 4096> chunk {};
		const drmp3_uint64 chunkFrames = chunk.size() / static_cast<std::size_t>(channels);
		while (true) {
			const drmp3_uint64 framesRead = drmp3_read_pcm_frames_s16(&mp3, chunkFrames, chunk.data());
			if (framesRead == 0)
				break;
			const std::size_t samplesRead = static_cast<std::size_t>(framesRead) * static_cast<std::size_t>(channels);
			pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + samplesRead);
		}
		drmp3_uninit(&mp3);

		if (pcm.empty())
			return nullptr;
	}

	auto buffer = std::make_shared<OpenAlBuffer>();
	ALuint ids[2] = { 0, 0 };
	alGetError();
	alGenBuffers(2, ids);
	if (alGetError() != AL_NO_ERROR || ids[0] == 0 || ids[1] == 0) {
		if (ids[0] != 0 || ids[1] != 0)
			alDeleteBuffers(2, ids);
		return nullptr;
	}
	buffer->monoId = ids[0];
	buffer->stereoId = ids[1];

	if (channels == 1) {
		alBufferData(buffer->monoId, AL_FORMAT_MONO16, pcm.data(), static_cast<ALsizei>(pcm.size() * sizeof(std::int16_t)), static_cast<ALsizei>(sampleRate));
		std::vector<std::int16_t> stereoPcm(pcm.size() * 2);
		for (std::size_t i = 0; i < pcm.size(); ++i) {
			stereoPcm[i * 2] = pcm[i];
			stereoPcm[i * 2 + 1] = pcm[i];
		}
		alBufferData(buffer->stereoId, AL_FORMAT_STEREO16, stereoPcm.data(), static_cast<ALsizei>(stereoPcm.size() * sizeof(std::int16_t)), static_cast<ALsizei>(sampleRate));
	} else {
		alBufferData(buffer->stereoId, AL_FORMAT_STEREO16, pcm.data(), static_cast<ALsizei>(pcm.size() * sizeof(std::int16_t)), static_cast<ALsizei>(sampleRate));
		const std::size_t frameCount = pcm.size() / 2;
		std::vector<std::int16_t> monoPcm(frameCount);
		for (std::size_t i = 0; i < frameCount; ++i) {
			monoPcm[i] = static_cast<std::int16_t>((static_cast<int>(pcm[i * 2]) + static_cast<int>(pcm[i * 2 + 1])) / 2);
		}
		alBufferData(buffer->monoId, AL_FORMAT_MONO16, monoPcm.data(), static_cast<ALsizei>(monoPcm.size() * sizeof(std::int16_t)), static_cast<ALsizei>(sampleRate));
	}

	if (alGetError() != AL_NO_ERROR) {
		return nullptr;
	}

	buffer->channels = channels;
	buffer->sampleRate = sampleRate;
	buffer->durationMs = static_cast<int>((pcm.size() / static_cast<std::size_t>(channels) * 1000ULL) / static_cast<std::size_t>(sampleRate));
	return buffer;
}

ALuint CreateOpenAlSource(ALuint bufferId)
{
	alGetError();
	ALuint source = 0;
	alGenSources(1, &source);
	if (alGetError() != AL_NO_ERROR || source == 0)
		return 0;

	alSourcei(source, AL_BUFFER, static_cast<ALint>(bufferId));
	alSourcef(source, AL_PITCH, 1.0F);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(source, AL_ROLLOFF_FACTOR, 0.0F);
	alSource3f(source, AL_POSITION, 0.0F, 0.0F, 0.0F);
	alSource3f(source, AL_VELOCITY, 0.0F, 0.0F, 0.0F);

	// Use OpenAL Soft's highest-quality 24-point band-limited sinc resampler (BSinc24)
	const ALint numResamplers = alGetInteger(AL_NUM_RESAMPLERS_SOFT);
	if (numResamplers > 0) {
		alSourcei(source, AL_SOURCE_RESAMPLER_SOFT, numResamplers - 1);
	}
	alGetError();
	return source;
}

} // namespace

SoundSample::~SoundSample()
{
	Release();
}

void SoundSample::Release()
{
	if (al_source_ != 0) {
		if (IsOpenAlActive()) {
			alSourceStop(al_source_);
			alSourcei(al_source_, AL_BUFFER, 0);
			alDeleteSources(1, &al_source_);
		}
		al_source_ = 0;
	}
	al_buffer_ = nullptr;
	stream_ = nullptr;
	file_data_ = nullptr;
	file_data_size_ = 0;
}

bool SoundSample::IsPlaying()
{
	if (al_source_ != 0 && IsOpenAlActive()) {
		ALint state = AL_STOPPED;
		alGetSourcei(al_source_, AL_SOURCE_STATE, &state);
		return state == AL_PLAYING;
	}
	return stream_ && stream_->isPlaying();
}

bool SoundSample::Play(int numIterations)
{
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSourcef(al_source_, AL_PITCH, 1.0F);
		alSourcei(al_source_, AL_LOOPING, numIterations == 0 ? AL_TRUE : AL_FALSE);
		alSourcePlay(al_source_);
		return alGetError() == AL_NO_ERROR;
	}
	if (!stream_ || !stream_->play(numIterations)) {
		LogError(LogCategory::Audio, "Aulib::Stream::play (from SoundSample::Play): {}", SDL_GetError());
		return false;
	}
	return true;
}

bool SoundSample::PlayWith3D(int logSoundVolume, int logUserVolume, int logPan, int deltaX, int deltaY, bool isPositional, bool applyReverb, bool isOccluded, bool loop)
{
	const int combinedLogVol = logSoundVolume + logUserVolume * (ATTENUATION_MIN / VOLUME_MIN);
	SetVolume(combinedLogVol, ATTENUATION_MIN, 0);

	if (al_source_ != 0 && al_buffer_ != nullptr && IsOpenAlActive()) {
		const bool use3DSpatial = isPositional && (deltaX != 0 || deltaY != 0);
		const ALuint targetBufferId = use3DSpatial ? al_buffer_->monoId : al_buffer_->stereoId;

		ALint currentBuffer = 0;
		alGetSourcei(al_source_, AL_BUFFER, &currentBuffer);
		if (static_cast<ALuint>(currentBuffer) != targetBufferId) {
			alSourceStop(al_source_);
			alSourcei(al_source_, AL_BUFFER, static_cast<ALint>(targetBufferId));
		}

		if (use3DSpatial) {
			// Map isometric tile offsets (deltaX, deltaY) onto the horizontal listener plane (posY = 0)
			// with a small front depth offset (posZ = -0.28F) whenever the sound is to the left or right
			// of the player (horiz != 0). This places melee monsters (|horiz| = 1..2) at +-82 to +-85 degrees
			// azimuth and distant monsters/barrels at +-85 to +-88 degrees azimuth regardless of vertical screen offset.
			const float horiz = static_cast<float>(deltaX - deltaY);
			const float vert = static_cast<float>(deltaX + deltaY);
			float posX = 0.0F;
			float posY = 0.0F;
			float posZ = -0.28F;
			if (horiz != 0.0F) {
				posX = std::copysign(1.20F + std::abs(horiz) * 0.90F, horiz);
			} else {
				posZ = (vert > 0.0F) ? 0.80F : -1.00F;
			}

			alSourcei(al_source_, AL_DIRECT_CHANNELS_SOFT, AL_FALSE);
			alSourcei(al_source_, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
			alSource3f(al_source_, AL_POSITION, posX, posY, posZ);
		} else {
			// 2D UI, inventory, town speech, and player-centered sounds (delta == 0,0) use the stereo buffer
			// with AL_REMIX_UNMATCHED_SOFT and AL_SOURCE_SPATIALIZE_SOFT=AL_FALSE to bypass HRTF coloration completely.
			alSourcei(al_source_, AL_DIRECT_CHANNELS_SOFT, AL_REMIX_UNMATCHED_SOFT);
			alSourcei(al_source_, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
			alSource3f(al_source_, AL_POSITION, 0.0F, 0.0F, 0.0F);
		}

		const ALuint efxSlot = (applyReverb && *sgOptions.Audio.efxReverb != ReverbLevel::Off) ? GetOpenAlEfxSlot() : 0;
		if (efxSlot != 0) {
			const ALuint directFilter = (use3DSpatial && isOccluded) ? GetOpenAlOccludedDirectFilter() : AL_FILTER_NULL;
			const ALuint sendFilter = use3DSpatial
			    ? (isOccluded ? GetOpenAlOccludedSendFilter() : AL_FILTER_NULL)
			    : GetOpenAlStereoSendFilter();
			alSourcei(al_source_, AL_DIRECT_FILTER, static_cast<ALint>(directFilter));
			alSource3i(al_source_, AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(efxSlot), 0, static_cast<ALint>(sendFilter));
		} else {
			alSourcei(al_source_, AL_DIRECT_FILTER, AL_FILTER_NULL);
			alSource3i(al_source_, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
		}

		return Play(loop ? 0 : 1);
	}

	SetStereoPosition(logPan);
	return Play(loop ? 0 : 1);
}

void SoundSample::Stop()
{
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSourceStop(al_source_);
		alSourcei(al_source_, AL_LOOPING, AL_FALSE);
		return;
	}
	if (stream_)
		stream_->stop();
}

void SoundSample::Mute()
{
	muted_ = true;
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSourcef(al_source_, AL_GAIN, 0.0F);
		return;
	}
	if (stream_)
		stream_->mute();
}

void SoundSample::Unmute()
{
	muted_ = false;
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSourcef(al_source_, AL_GAIN, saved_gain_);
		return;
	}
	if (stream_)
		stream_->unmute();
}

int SoundSample::DuplicateFrom(const SoundSample &other)
{
	if (other.al_buffer_ != nullptr && IsOpenAlActive()) {
		Release();
		al_buffer_ = other.al_buffer_;
		al_source_ = CreateOpenAlSource(al_buffer_->stereoId);
		if (al_source_ == 0) {
			al_buffer_ = nullptr;
			return -1;
		}
		return 0;
	}
	if (other.IsStreaming())
		return SetChunkStream(other.file_path_, other.isMp3_);
	return SetChunk(other.file_data_, other.file_data_size_, other.isMp3_);
}

int SoundSample::SetChunkStream(std::string filePath, bool isMp3, bool logErrors)
{
	Release();

	// Decode speech/boss taunt and ambient weather loop streams (< 16 MB) directly into OpenAL Soft
	if (IsOpenAlActive()) {
		AssetRef ref = FindAsset(filePath.c_str());
		if (ref.ok() && ref.size() > 0 && ref.size() <= 16 * 1024 * 1024) {
			const std::size_t size = ref.size();
			AssetHandle assetHandle = OpenAsset(std::move(ref));
			if (assetHandle.ok()) {
				auto rawData = MakeArraySharedPtr<std::uint8_t>(size);
				if (assetHandle.read(rawData.get(), size)) {
					al_buffer_ = DecodeMemoryToOpenAlBuffer(rawData.get(), size, isMp3);
					if (al_buffer_ != nullptr) {
						al_source_ = CreateOpenAlSource(al_buffer_->stereoId);
						if (al_source_ != 0) {
							file_path_ = std::move(filePath);
							isMp3_ = isMp3;
							return 0;
						}
						al_buffer_ = nullptr;
					}
				}
			}
		}
	}

	SDL_RWops *handle = OpenAssetAsSdlRwOps(filePath.c_str(), /*threadsafe=*/true);
	if (handle == nullptr) {
		if (logErrors)
			LogError(LogCategory::Audio, "OpenAsset failed (from SoundSample::SetChunkStream) for {}: {}", filePath, SDL_GetError());
		return -1;
	}
	file_path_ = std::move(filePath);
	isMp3_ = isMp3;
	stream_ = CreateStream(handle, isMp3);
	if (!stream_ || !stream_->open()) {
		stream_ = nullptr;
		if (logErrors)
			LogError(LogCategory::Audio, "Aulib::Stream::open (from SoundSample::SetChunkStream) for {}: {}", file_path_, SDL_GetError());
		return -1;
	}
	return 0;
}

int SoundSample::SetChunk(ArraySharedPtr<std::uint8_t> fileData, std::size_t dwBytes, bool isMp3)
{
	Release();
	isMp3_ = isMp3;

	if (IsOpenAlActive()) {
		al_buffer_ = DecodeMemoryToOpenAlBuffer(fileData.get(), dwBytes, isMp3_);
		if (al_buffer_ != nullptr) {
			al_source_ = CreateOpenAlSource(al_buffer_->stereoId);
			if (al_source_ != 0) {
				return 0;
			}
			al_buffer_ = nullptr;
		}
	}

	file_data_ = std::move(fileData);
	file_data_size_ = dwBytes;
	SDL_RWops *buf = SDL_RWFromConstMem(file_data_.get(), static_cast<int>(dwBytes));
	if (buf == nullptr) {
		return -1;
	}

	stream_ = CreateStream(buf, isMp3_);
	if (!stream_ || !stream_->open()) {
		stream_ = nullptr;
		file_data_ = nullptr;
		LogError(LogCategory::Audio, "Aulib::Stream::open (from SoundSample::SetChunk): {}", SDL_GetError());
		return -1;
	}

	return 0;
}

void SoundSample::SetVolume(int logVolume, int logMin, int logMax)
{
	saved_gain_ = clamp(VolumeLogToLinear(logVolume, logMin, logMax), 0.0F, 1.0F);
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSourcef(al_source_, AL_GAIN, muted_ ? 0.0F : saved_gain_);
		return;
	}
	if (stream_)
		stream_->setVolume(saved_gain_);
}

void SoundSample::SetStereoPosition(int logPan)
{
	const float panLinear = PanLogToLinear(logPan);
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSource3f(al_source_, AL_POSITION, panLinear * 2.5F, 0.0F, -2.0F);
		return;
	}
	if (stream_)
		stream_->setStereoPosition(panLinear);
}

int SoundSample::GetLength() const
{
	if (al_buffer_ != nullptr)
		return al_buffer_->durationMs;
	if (!stream_)
		return 0;
	return std::chrono::duration_cast<std::chrono::milliseconds>(stream_->duration()).count();
}

} // namespace devilution
