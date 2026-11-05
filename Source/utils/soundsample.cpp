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
	ALuint id = 0;
	int durationMs = 0;
	int channels = 1;
	int sampleRate = 22050;

	~OpenAlBuffer()
	{
		if (id != 0 && IsOpenAlActive()) {
			alDeleteBuffers(1, &id);
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

std::shared_ptr<OpenAlBuffer> DecodeToOpenAlBuffer(SDL_RWops *handle, bool isMp3)
{
	auto decoder = CreateDecoder(isMp3);
	if (!decoder->open(handle)) {
		SDL_RWclose(handle);
		return nullptr;
	}

	const int channels = decoder->getChannels();
	const int sampleRate = decoder->getRate();
	if ((channels != 1 && channels != 2) || sampleRate <= 0) {
		SDL_RWclose(handle);
		return nullptr;
	}

	std::vector<std::int16_t> pcm;
	pcm.reserve(32768);
	std::array<float, 4096> chunk {};
	bool callAgain = true;
	while (callAgain) {
		const int decoded = decoder->decode(chunk.data(), static_cast<int>(chunk.size()), callAgain);
		if (decoded <= 0)
			break;
		const std::size_t oldSize = pcm.size();
		pcm.resize(oldSize + static_cast<std::size_t>(decoded));
		for (int i = 0; i < decoded; ++i) {
			const float clamped = clamp(chunk[static_cast<std::size_t>(i)], -1.0F, 1.0F);
			pcm[oldSize + static_cast<std::size_t>(i)] = static_cast<std::int16_t>(clamped * 32767.0F);
		}
	}

	SDL_RWclose(handle);

	if (pcm.empty())
		return nullptr;

	auto buffer = std::make_shared<OpenAlBuffer>();
	alGetError();
	alGenBuffers(1, &buffer->id);
	if (alGetError() != AL_NO_ERROR || buffer->id == 0)
		return nullptr;

	const ALenum format = (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	alBufferData(buffer->id, format, pcm.data(), static_cast<ALsizei>(pcm.size() * sizeof(std::int16_t)), static_cast<ALsizei>(sampleRate));
	if (alGetError() != AL_NO_ERROR) {
		alDeleteBuffers(1, &buffer->id);
		buffer->id = 0;
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
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcef(source, AL_ROLLOFF_FACTOR, 0.0F);
	alSource3f(source, AL_POSITION, 0.0F, 0.0F, -2.0F);
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

bool SoundSample::PlayWith3D(int logSoundVolume, int logUserVolume, int logPan, int deltaX, int deltaY, bool isPositional, bool applyReverb, bool isOccluded)
{
	const int combinedLogVol = logSoundVolume + logUserVolume * (ATTENUATION_MIN / VOLUME_MIN);
	SetVolume(combinedLogVol, ATTENUATION_MIN, 0);

	if (al_source_ != 0 && IsOpenAlActive()) {
		if (isPositional) {
			// Map isometric tile offsets (deltaX, deltaY) to listener-relative 3D coordinates:
			// Screen X (left/right) = (deltaX - deltaY) * 0.7071
			// Screen Y (up/down)    = (deltaX + deltaY) * 0.7071
			const float isoX = static_cast<float>(deltaX - deltaY) * 0.70710678F * 0.35F;
			const float isoY = static_cast<float>(deltaX + deltaY) * 0.70710678F * 0.35F;
			const float posX = isoX;
			const float posY = -isoY * 0.30F;
			const float posZ = isoY * 0.75F - 2.2F;

			alSourcei(al_source_, AL_DIRECT_CHANNELS_SOFT, AL_FALSE);
			alSourcei(al_source_, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
			alSource3f(al_source_, AL_POSITION, posX, posY, posZ);
		} else {
			const float panLinear = PanLogToLinear(logPan);
			if (std::abs(panLinear) < 0.001F && !applyReverb) {
				alSourcei(al_source_, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
				alSourcei(al_source_, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
				alSource3f(al_source_, AL_POSITION, 0.0F, 0.0F, 0.0F);
			} else {
				alSourcei(al_source_, AL_DIRECT_CHANNELS_SOFT, AL_FALSE);
				alSourcei(al_source_, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
				alSource3f(al_source_, AL_POSITION, panLinear * 2.5F, 0.0F, -2.0F);
			}
		}

		const ALuint efxSlot = (applyReverb && *sgOptions.Audio.efxReverb) ? GetOpenAlEfxSlot() : 0;
		if (efxSlot != 0) {
			const ALuint directFilter = isOccluded ? GetOpenAlOccludedDirectFilter() : AL_FILTER_NULL;
			const ALuint sendFilter = isOccluded ? GetOpenAlOccludedSendFilter() : AL_FILTER_NULL;
			alSourcei(al_source_, AL_DIRECT_FILTER, static_cast<ALint>(directFilter));
			alSource3i(al_source_, AL_AUXILIARY_SEND_FILTER, static_cast<ALint>(efxSlot), 0, static_cast<ALint>(sendFilter));
		} else {
			alSourcei(al_source_, AL_DIRECT_FILTER, AL_FILTER_NULL);
			alSource3i(al_source_, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
		}

		return Play(1);
	}

	SetStereoPosition(logPan);
	return Play(1);
}

void SoundSample::Stop()
{
	if (al_source_ != 0 && IsOpenAlActive()) {
		alSourceStop(al_source_);
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
		al_source_ = CreateOpenAlSource(al_buffer_->id);
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

	// Decode short speech/boss taunt streams (< 2 MB) into OpenAL Soft so they benefit from EFX reverb in dungeons
	if (IsOpenAlActive()) {
		AssetRef ref = FindAsset(filePath.c_str());
		if (ref.ok() && ref.size() > 0 && ref.size() <= 2 * 1024 * 1024) {
			SDL_RWops *rwHandle = OpenAssetAsSdlRwOps(filePath.c_str(), /*threadsafe=*/true);
			if (rwHandle != nullptr) {
				al_buffer_ = DecodeToOpenAlBuffer(rwHandle, isMp3);
				if (al_buffer_ != nullptr) {
					al_source_ = CreateOpenAlSource(al_buffer_->id);
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
		SDL_RWops *buf = SDL_RWFromConstMem(fileData.get(), static_cast<int>(dwBytes));
		if (buf != nullptr) {
			al_buffer_ = DecodeToOpenAlBuffer(buf, isMp3_);
			if (al_buffer_ != nullptr) {
				al_source_ = CreateOpenAlSource(al_buffer_->id);
				if (al_source_ != 0) {
					return 0;
				}
				al_buffer_ = nullptr;
			}
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
