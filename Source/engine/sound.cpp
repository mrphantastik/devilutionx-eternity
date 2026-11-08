/**
 * @file sound.cpp
 *
 * Implementation of functions setting up the audio pipeline (OpenAL Soft + EFX + HRTF).
 */
#ifndef AL_LIBTYPE_STATIC
#define AL_LIBTYPE_STATIC
#endif
#ifndef AL_ALEXT_PROTOTYPES
#define AL_ALEXT_PROTOTYPES
#endif

#include "engine/sound.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx-presets.h>
#include <AL/efx.h>
#include <SDL.h>

#include "engine/assets.hpp"
#include "init.h"
#include "options.h"
#include "utils/log.hpp"
#include "utils/math.h"
#include "utils/sdl_mutex.h"
#include "utils/stdcompat/algorithm.hpp"
#include "utils/stdcompat/optional.hpp"
#include "utils/stdcompat/shared_ptr_array.hpp"
#include "utils/str_cat.hpp"
#include "utils/stubs.h"

namespace devilution {

bool gbSndInited;
/** The active background music track id. */
_music_id sgnMusicTrack = NUM_MUSIC;

bool gbMusicOn = true;
/** Specifies whether sound effects are enabled. */
bool gbSoundOn = true;

namespace {

ALCdevice *gAlDevice = nullptr;
ALCcontext *gAlContext = nullptr;
ALuint gEfxSlot = 0;
ALuint gEfxEffect = 0;
ALuint gFilterOccludedDirect = 0;
ALuint gFilterOccludedSend = 0;
ALuint gFilterStereoSend = 0;
dungeon_type gActiveReverbDungeon = DTYPE_NONE;

float GetOpenAlReverbGain()
{
	switch (*sgOptions.Audio.efxReverb) {
	case ReverbLevel::High:
		return 1.0F;
	case ReverbLevel::Normal:
		return 0.45F;
	case ReverbLevel::Off:
	default:
		return 0.0F;
	}
}

void LoadEaxReverbPreset(ALuint effect, const EFXEAXREVERBPROPERTIES &preset, float effectGain, float lateReverbScale)
{
	const bool isHigh = (*sgOptions.Audio.efxReverb == ReverbLevel::High);
	const float gainScale = isHigh ? 1.00F : 0.68F;
	const float reflScale = isHigh ? 1.50F : 0.75F;
	const float lateScale = lateReverbScale * (isHigh ? 1.25F : 0.50F);
	const float decayScale = isHigh ? 1.20F : 0.75F;
	const float scaledDecayTime = clamp(preset.flDecayTime * decayScale, 0.1F, 20.0F);

	alGetError();
	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
	if (alGetError() == AL_NO_ERROR) {
		alEffectf(effect, AL_EAXREVERB_DENSITY, preset.flDensity);
		alEffectf(effect, AL_EAXREVERB_DIFFUSION, preset.flDiffusion);
		alEffectf(effect, AL_EAXREVERB_GAIN, clamp(effectGain * gainScale, 0.0F, 1.0F));
		alEffectf(effect, AL_EAXREVERB_GAINHF, preset.flGainHF);
		alEffectf(effect, AL_EAXREVERB_GAINLF, preset.flGainLF);
		alEffectf(effect, AL_EAXREVERB_DECAY_TIME, scaledDecayTime);
		alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, preset.flDecayHFRatio);
		alEffectf(effect, AL_EAXREVERB_DECAY_LFRATIO, preset.flDecayLFRatio);
		alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, clamp(preset.flReflectionsGain * reflScale, 0.0F, 3.16F));
		alEffectf(effect, AL_EAXREVERB_REFLECTIONS_DELAY, preset.flReflectionsDelay);
		alEffectfv(effect, AL_EAXREVERB_REFLECTIONS_PAN, preset.flReflectionsPan);
		alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, clamp(preset.flLateReverbGain * lateScale, 0.0F, 10.0F));
		alEffectf(effect, AL_EAXREVERB_LATE_REVERB_DELAY, preset.flLateReverbDelay);
		alEffectfv(effect, AL_EAXREVERB_LATE_REVERB_PAN, preset.flLateReverbPan);
		alEffectf(effect, AL_EAXREVERB_ECHO_TIME, preset.flEchoTime);
		alEffectf(effect, AL_EAXREVERB_ECHO_DEPTH, preset.flEchoDepth);
		alEffectf(effect, AL_EAXREVERB_MODULATION_TIME, preset.flModulationTime);
		alEffectf(effect, AL_EAXREVERB_MODULATION_DEPTH, preset.flModulationDepth);
		alEffectf(effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, preset.flAirAbsorptionGainHF);
		alEffectf(effect, AL_EAXREVERB_HFREFERENCE, preset.flHFReference);
		alEffectf(effect, AL_EAXREVERB_LFREFERENCE, preset.flLFReference);
		alEffectf(effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, preset.flRoomRolloffFactor);
		alEffecti(effect, AL_EAXREVERB_DECAY_HFLIMIT, preset.iDecayHFLimit);
		return;
	}

	alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
	alEffectf(effect, AL_REVERB_DENSITY, preset.flDensity);
	alEffectf(effect, AL_REVERB_DIFFUSION, preset.flDiffusion);
	alEffectf(effect, AL_REVERB_GAIN, clamp(effectGain * gainScale, 0.0F, 1.0F));
	alEffectf(effect, AL_REVERB_GAINHF, preset.flGainHF);
	alEffectf(effect, AL_REVERB_DECAY_TIME, scaledDecayTime);
	alEffectf(effect, AL_REVERB_DECAY_HFRATIO, preset.flDecayHFRatio);
	alEffectf(effect, AL_REVERB_REFLECTIONS_GAIN, clamp(preset.flReflectionsGain * reflScale, 0.0F, 3.16F));
	alEffectf(effect, AL_REVERB_REFLECTIONS_DELAY, preset.flReflectionsDelay);
	alEffectf(effect, AL_REVERB_LATE_REVERB_GAIN, clamp(preset.flLateReverbGain * lateScale, 0.0F, 10.0F));
	alEffectf(effect, AL_REVERB_LATE_REVERB_DELAY, preset.flLateReverbDelay);
	alEffectf(effect, AL_REVERB_AIR_ABSORPTION_GAINHF, preset.flAirAbsorptionGainHF);
	alEffectf(effect, AL_REVERB_ROOM_ROLLOFF_FACTOR, preset.flRoomRolloffFactor);
	alEffecti(effect, AL_REVERB_DECAY_HFLIMIT, preset.iDecayHFLimit);
}

void InitOpenAl()
{
	const std::string devOpt = *sgOptions.Audio.device;
	gAlDevice = alcOpenDevice(devOpt.empty() ? nullptr : devOpt.c_str());
	if (gAlDevice == nullptr && !devOpt.empty()) {
		gAlDevice = alcOpenDevice(nullptr);
	}
	if (gAlDevice == nullptr) {
		LogError(LogCategory::Audio, "Failed to open OpenAL audio device");
		return;
	}

	const ALCint sampleRate = static_cast<ALCint>(std::max<std::uint32_t>(*sgOptions.Audio.sampleRate, 48000U));
	const ALCint attrs[] = {
		ALC_FREQUENCY, sampleRate,
		ALC_MAX_AUXILIARY_SENDS, 2,
		ALC_HRTF_SOFT, *sgOptions.Audio.hrtf ? ALC_TRUE : ALC_FALSE,
		0, 0
	};

	gAlContext = alcCreateContext(gAlDevice, attrs);
	if (gAlContext == nullptr || alcMakeContextCurrent(gAlContext) == ALC_FALSE) {
		LogError(LogCategory::Audio, "Failed to create or activate OpenAL context");
		if (gAlContext != nullptr) {
			alcDestroyContext(gAlContext);
			gAlContext = nullptr;
		}
		alcCloseDevice(gAlDevice);
		gAlDevice = nullptr;
		return;
	}

	alDopplerFactor(0.0F);
	const ALfloat listenerOri[] = { 0.0F, 0.0F, -1.0F, 0.0F, 1.0F, 0.0F };
	alListener3f(AL_POSITION, 0.0F, 0.0F, 0.0F);
	alListener3f(AL_VELOCITY, 0.0F, 0.0F, 0.0F);
	alListenerfv(AL_ORIENTATION, listenerOri);

	if (alcIsExtensionPresent(gAlDevice, "ALC_EXT_EFX") == ALC_TRUE) {
		alGetError();
		alGenAuxiliaryEffectSlots(1, &gEfxSlot);
		alGenEffects(1, &gEfxEffect);
		if (alGetError() != AL_NO_ERROR) {
			gEfxSlot = 0;
			gEfxEffect = 0;
		} else {
			alAuxiliaryEffectSlotf(gEfxSlot, AL_EFFECTSLOT_GAIN, GetOpenAlReverbGain());
			alAuxiliaryEffectSloti(gEfxSlot, AL_EFFECTSLOT_AUXILIARY_SEND_AUTO, AL_TRUE);
		}

		alGenFilters(1, &gFilterOccludedDirect);
		if (alGetError() == AL_NO_ERROR && gFilterOccludedDirect != 0) {
			alFilteri(gFilterOccludedDirect, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
			alFilterf(gFilterOccludedDirect, AL_LOWPASS_GAIN, 0.72F);
			alFilterf(gFilterOccludedDirect, AL_LOWPASS_GAINHF, 0.48F);
		} else {
			gFilterOccludedDirect = 0;
		}

		alGenFilters(1, &gFilterOccludedSend);
		if (alGetError() == AL_NO_ERROR && gFilterOccludedSend != 0) {
			alFilteri(gFilterOccludedSend, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
			alFilterf(gFilterOccludedSend, AL_LOWPASS_GAIN, 0.85F);
			alFilterf(gFilterOccludedSend, AL_LOWPASS_GAINHF, 0.55F);
		} else {
			gFilterOccludedSend = 0;
		}

		alGenFilters(1, &gFilterStereoSend);
		if (alGetError() == AL_NO_ERROR && gFilterStereoSend != 0) {
			alFilteri(gFilterStereoSend, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
			alFilterf(gFilterStereoSend, AL_LOWPASS_GAIN, 0.50F);
			alFilterf(gFilterStereoSend, AL_LOWPASS_GAINHF, 1.00F);
		} else {
			gFilterStereoSend = 0;
		}

		gActiveReverbDungeon = DTYPE_NONE;
		UpdateSoundEnvironment(DTYPE_TOWN);
	}
}

void DeinitOpenAl()
{
	if (gAlContext == nullptr && gAlDevice == nullptr)
		return;

	if (gFilterOccludedDirect != 0) {
		alDeleteFilters(1, &gFilterOccludedDirect);
		gFilterOccludedDirect = 0;
	}
	if (gFilterOccludedSend != 0) {
		alDeleteFilters(1, &gFilterOccludedSend);
		gFilterOccludedSend = 0;
	}
	if (gFilterStereoSend != 0) {
		alDeleteFilters(1, &gFilterStereoSend);
		gFilterStereoSend = 0;
	}
	if (gEfxSlot != 0) {
		alAuxiliaryEffectSloti(gEfxSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
		alDeleteAuxiliaryEffectSlots(1, &gEfxSlot);
		gEfxSlot = 0;
	}
	if (gEfxEffect != 0) {
		alDeleteEffects(1, &gEfxEffect);
		gEfxEffect = 0;
	}

	alcMakeContextCurrent(nullptr);
	if (gAlContext != nullptr) {
		alcDestroyContext(gAlContext);
		gAlContext = nullptr;
	}
	if (gAlDevice != nullptr) {
		alcCloseDevice(gAlDevice);
		gAlDevice = nullptr;
	}
	gActiveReverbDungeon = DTYPE_NONE;
}

SoundSample music;

std::string GetMp3Path(const char *path)
{
	std::string mp3Path = path;
	const std::string::size_type dot = mp3Path.find_last_of('.');
	mp3Path.replace(dot + 1, mp3Path.size() - (dot + 1), "mp3");
	return mp3Path;
}

bool LoadAudioFile(const char *path, bool stream, bool errorDialog, SoundSample &result)
{
	bool isMp3 = true;
	std::string foundPath = GetMp3Path(path);
	AssetRef ref = FindAsset(foundPath.c_str());
	if (!ref.ok()) {
		ref = FindAsset(path);
		foundPath = path;
		isMp3 = false;
	}
	if (!ref.ok()) {
		if (errorDialog)
			ErrDlg("Audio file not found", StrCat(path, "\n", SDL_GetError(), "\n"), __FILE__, __LINE__);
		return false;
	}

#ifdef STREAM_ALL_AUDIO_MIN_FILE_SIZE
#if STREAM_ALL_AUDIO_MIN_FILE_SIZE == 0
	stream = true;
#else
	size_t size;
	if (!stream) {
		size = ref.size();
		stream = size >= STREAM_ALL_AUDIO_MIN_FILE_SIZE;
	}
#endif
#endif

	if (stream) {
		if (result.SetChunkStream(foundPath, isMp3, /*logErrors=*/true) != 0) {
			if (errorDialog) {
				ErrDlg("Failed to load audio file", StrCat(foundPath, "\n", SDL_GetError(), "\n"), __FILE__, __LINE__);
			}
			return false;
		}
	} else {
#if !defined(STREAM_ALL_AUDIO_MIN_FILE_SIZE) || STREAM_ALL_AUDIO_MIN_FILE_SIZE == 0
		const size_t size = ref.size();
#endif
		AssetHandle handle = OpenAsset(std::move(ref));
		if (!handle.ok()) {
			if (errorDialog)
				ErrDlg("Failed to load audio file", StrCat(foundPath, "\n", SDL_GetError(), "\n"), __FILE__, __LINE__);
			return false;
		}
		auto waveFile = MakeArraySharedPtr<std::uint8_t>(size);
		if (!handle.read(waveFile.get(), size)) {
			if (errorDialog)
				ErrDlg("Failed to read file", StrCat(foundPath, ": ", SDL_GetError()), __FILE__, __LINE__);
			return false;
		}
		const int error = result.SetChunk(waveFile, size, isMp3);
		if (error != 0) {
			if (errorDialog)
				ErrSdl();
			return false;
		}
	}
	return true;
}

std::list<std::unique_ptr<SoundSample>> duplicateSounds;
std::optional<SdlMutex> duplicateSoundsMutex;
constexpr size_t MaxActiveDuplicateSounds = 32;
constexpr size_t MaxPooledDuplicateSounds = 64;
uint32_t lastDuplicateTick = 0;

SoundSample *DuplicateSound(const SoundSample &sound)
{
	const uint32_t tc = SDL_GetTicks();
	if (tc - lastDuplicateTick < 5) {
		return nullptr;
	}

	{
		const std::lock_guard<SdlMutex> lock(*duplicateSoundsMutex);
		SoundSample *reusable = nullptr;
		size_t activeCount = 0;
		for (auto &dup : duplicateSounds) {
			if (dup->IsPlaying()) {
				activeCount++;
			} else if (reusable == nullptr && !sound.IsStreaming() && dup->MatchesData(sound)) {
				reusable = dup.get();
			}
		}

		if (activeCount >= MaxActiveDuplicateSounds) {
			return nullptr;
		}

		if (reusable != nullptr) {
			lastDuplicateTick = tc;
			return reusable;
		}

		if (duplicateSounds.size() >= MaxPooledDuplicateSounds) {
			for (auto it = duplicateSounds.begin(); it != duplicateSounds.end(); ++it) {
				if (!(*it)->IsPlaying()) {
					duplicateSounds.erase(it);
					break;
				}
			}
		}
	}

	auto duplicate = std::make_unique<SoundSample>();
	if (duplicate->DuplicateFrom(sound) != 0)
		return nullptr;
	auto *result = duplicate.get();
	{
		const std::lock_guard<SdlMutex> lock(*duplicateSoundsMutex);
		duplicateSounds.push_back(std::move(duplicate));
	}
	lastDuplicateTick = tc;
	return result;
}

/** Maps from track ID to track name in spawn. */
const char *const SpawnMusicTracks[NUM_MUSIC] = {
	"music\\stowne.wav",
	"music\\slvla.wav",
	"music\\slvla.wav",
	"music\\slvla.wav",
	"music\\slvla.wav",
	"music\\dlvlf.wav",
	"music\\dlvle.wav",
	"music\\sintro.wav",
};
/** Maps from track ID to track name. */
const char *const MusicTracks[NUM_MUSIC] = {
	"music\\dtowne.wav",
	"music\\dlvla.wav",
	"music\\dlvlb.wav",
	"music\\dlvlc.wav",
	"music\\dlvld.wav",
	"music\\dlvlf.wav",
	"music\\dlvle.wav",
	"music\\dintro.wav",
};

int CapVolume(int volume)
{
	return clamp(volume, VOLUME_MIN, VOLUME_MAX);
}

} // namespace

bool IsOpenAlActive()
{
	return gAlContext != nullptr;
}

uint32_t GetOpenAlEfxSlot()
{
	return gEfxSlot;
}

uint32_t GetOpenAlOccludedDirectFilter()
{
	return gFilterOccludedDirect;
}

uint32_t GetOpenAlOccludedSendFilter()
{
	return gFilterOccludedSend;
}

uint32_t GetOpenAlStereoSendFilter()
{
	return gFilterStereoSend;
}

void ApplyOpenAlReverbOption()
{
	if (!IsOpenAlActive() || gEfxSlot == 0 || gEfxEffect == 0)
		return;
	const dungeon_type targetDungeon = (gActiveReverbDungeon != DTYPE_NONE) ? gActiveReverbDungeon : DTYPE_TOWN;
	gActiveReverbDungeon = DTYPE_NONE;
	UpdateSoundEnvironment(targetDungeon);
}

void ApplyOpenAlHrtfOption()
{
	if (!IsOpenAlActive() || gAlDevice == nullptr)
		return;
	const ALCint sampleRate = static_cast<ALCint>(std::max<std::uint32_t>(*sgOptions.Audio.sampleRate, 48000U));
	const ALCint attrs[] = {
		ALC_FREQUENCY, sampleRate,
		ALC_MAX_AUXILIARY_SENDS, 2,
		ALC_HRTF_SOFT, *sgOptions.Audio.hrtf ? ALC_TRUE : ALC_FALSE,
		0, 0
	};
	alcResetDeviceSOFT(gAlDevice, attrs);
}

void UpdateSoundEnvironment(dungeon_type dungeonType)
{
	if (!IsOpenAlActive() || gEfxSlot == 0 || gEfxEffect == 0)
		return;
	if (gActiveReverbDungeon == dungeonType)
		return;

	gActiveReverbDungeon = dungeonType;

	EFXEAXREVERBPROPERTIES preset = EFX_REVERB_PRESET_GENERIC;
	float effectGain = 0.85F;
	float lateReverbScale = 1.50F;

	switch (dungeonType) {
	case DTYPE_TOWN:
		preset = EFX_REVERB_PRESET_OUTDOORS_BACKYARD;
		effectGain = 0.55F;
		lateReverbScale = 1.60F;
		break;
	case DTYPE_CATHEDRAL:
		preset = EFX_REVERB_PRESET_CHAPEL;
		effectGain = 0.92F;
		lateReverbScale = 1.55F;
		break;
	case DTYPE_CATACOMBS:
		preset = EFX_REVERB_PRESET_STONEROOM;
		effectGain = 0.95F;
		lateReverbScale = 1.65F;
		break;
	case DTYPE_CAVES:
		preset = EFX_REVERB_PRESET_CAVE;
		effectGain = 0.95F;
		lateReverbScale = 1.55F;
		break;
	case DTYPE_HELL:
		preset = EFX_REVERB_PRESET_CASTLE_LARGEROOM;
		preset.flDecayTime = 3.6F;
		preset.flDecayLFRatio = 1.30F;
		effectGain = 1.00F;
		lateReverbScale = 1.65F;
		break;
	case DTYPE_NEST:
		preset = EFX_REVERB_PRESET_CAVE;
		preset.flDecayTime = 1.60F;
		preset.flDecayHFRatio = 0.38F;
		preset.flDiffusion = 0.78F;
		effectGain = 0.90F;
		lateReverbScale = 1.55F;
		break;
	case DTYPE_CRYPT:
		preset = EFX_REVERB_PRESET_AUDITORIUM;
		preset.flDecayTime = 3.20F;
		preset.flDecayHFRatio = 0.95F;
		effectGain = 0.95F;
		lateReverbScale = 1.65F;
		break;
	default:
		preset = EFX_REVERB_PRESET_OUTDOORS_BACKYARD;
		effectGain = 0.50F;
		lateReverbScale = 1.40F;
		break;
	}

	LoadEaxReverbPreset(gEfxEffect, preset, effectGain, lateReverbScale);
	alAuxiliaryEffectSloti(gEfxSlot, AL_EFFECTSLOT_EFFECT, static_cast<ALint>(gEfxEffect));
	alAuxiliaryEffectSlotf(gEfxSlot, AL_EFFECTSLOT_GAIN, GetOpenAlReverbGain());
}

void ClearDuplicateSounds()
{
	if (!duplicateSoundsMutex)
		return;
	const std::lock_guard<SdlMutex> lock(*duplicateSoundsMutex);
	duplicateSounds.clear();
}

void snd_play_snd_ex(TSnd *pSnd, int lVolume, int lPan, int deltaX, int deltaY, bool isPositional, bool applyReverb, bool isOccluded)
{
	if (pSnd == nullptr || !gbSoundOn) {
		return;
	}

	uint32_t tc = SDL_GetTicks();
	if (tc - pSnd->start_tc < 80) {
		return;
	}

	SoundSample *sound = &pSnd->DSB;
	if (sound->IsPlaying()) {
		sound = DuplicateSound(*sound);
		if (sound == nullptr)
			return;
	}

	sound->PlayWith3D(lVolume, *sgOptions.Audio.soundVolume, lPan, deltaX, deltaY, isPositional, applyReverb, isOccluded);
	pSnd->start_tc = tc;
}

void snd_play_snd(TSnd *pSnd, int lVolume, int lPan)
{
	snd_play_snd_ex(pSnd, lVolume, lPan, 0, 0, false, false, false);
}

std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream)
{
	auto snd = std::make_unique<TSnd>();
	snd->start_tc = SDL_GetTicks() - 80 - 1;
#ifndef NOSOUND
	LoadAudioFile(path, stream, /*errorDialog=*/false, snd->DSB);
#endif
	return snd;
}

TSnd::~TSnd()
{
	if (DSB.IsLoaded())
		DSB.Stop();
	DSB.Release();
}

void snd_init()
{
	sgOptions.Audio.soundVolume.SetValue(CapVolume(*sgOptions.Audio.soundVolume));
	gbSoundOn = *sgOptions.Audio.soundVolume > VOLUME_MIN;
	sgbSaveSoundOn = gbSoundOn;

	sgOptions.Audio.musicVolume.SetValue(CapVolume(*sgOptions.Audio.musicVolume));
	gbMusicOn = *sgOptions.Audio.musicVolume > VOLUME_MIN;

	// Initialize OpenAL Soft (3D spatialization, Binaural HRTF, and EFX environmental reverb)
	InitOpenAl();

	// Initialize SDL_audiolib for streaming music and Smacker video playback
	if (!Aulib::init(*sgOptions.Audio.sampleRate, AUDIO_S16, *sgOptions.Audio.channels, *sgOptions.Audio.bufferSize, *sgOptions.Audio.device)) {
		LogError(LogCategory::Audio, "Failed to initialize audio (Aulib::init): {}", SDL_GetError());
		if (!IsOpenAlActive())
			return;
	}

	duplicateSoundsMutex.emplace();
	gbSndInited = true;
}

void snd_deinit()
{
	if (gbSndInited) {
		music.Release();
		ClearDuplicateSounds();
		DeinitOpenAl();
		Aulib::quit();
		duplicateSoundsMutex = std::nullopt;
	}

	gbSndInited = false;
}

_music_id GetLevelMusic(dungeon_type dungeonType)
{
	switch (dungeonType) {
	case DTYPE_TOWN:
		return TMUSIC_TOWN;
	case DTYPE_CATHEDRAL:
		return TMUSIC_CATHEDRAL;
	case DTYPE_CATACOMBS:
		return TMUSIC_CATACOMBS;
	case DTYPE_CAVES:
		return TMUSIC_CAVES;
	case DTYPE_HELL:
		return TMUSIC_HELL;
	case DTYPE_NEST:
		return TMUSIC_NEST;
	case DTYPE_CRYPT:
		return TMUSIC_CRYPT;
	default:
		return TMUSIC_INTRO;
	}
}

void music_stop()
{
	music.Release();
	sgnMusicTrack = NUM_MUSIC;
}

void music_start(_music_id nTrack)
{
	const char *trackPath;

	assert(nTrack < NUM_MUSIC);
	music_stop();
	if (!gbMusicOn)
		return;
	if (HaveSpawn())
		trackPath = SpawnMusicTracks[nTrack];
	else
		trackPath = MusicTracks[nTrack];

#ifdef DISABLE_STREAMING_MUSIC
	const bool stream = false;
#else
	const bool stream = true;
#endif
	if (!LoadAudioFile(trackPath, stream, /*errorDialog=*/false, music)) {
		music_stop();
		return;
	}

	music.SetVolume(*sgOptions.Audio.musicVolume, VOLUME_MIN, VOLUME_MAX);
	if (!diablo_is_focused())
		music_mute();
	if (!music.Play(/*numIterations=*/0)) {
		LogError(LogCategory::Audio, "Aulib::Stream::play (from music_start): {}", SDL_GetError());
		music_stop();
		return;
	}

	sgnMusicTrack = nTrack;
}

void sound_disable_music(bool disable)
{
	if (disable) {
		music_stop();
	} else if (sgnMusicTrack != NUM_MUSIC) {
		music_start(sgnMusicTrack);
	}
}

int sound_get_or_set_music_volume(int volume)
{
	if (volume == 1)
		return *sgOptions.Audio.musicVolume;

	sgOptions.Audio.musicVolume.SetValue(volume);

	if (music.IsLoaded())
		music.SetVolume(*sgOptions.Audio.musicVolume, VOLUME_MIN, VOLUME_MAX);

	return *sgOptions.Audio.musicVolume;
}

int sound_get_or_set_sound_volume(int volume)
{
	if (volume == 1)
		return *sgOptions.Audio.soundVolume;

	sgOptions.Audio.soundVolume.SetValue(volume);

	return *sgOptions.Audio.soundVolume;
}

void music_mute()
{
	if (music.IsLoaded())
		music.Mute();
}

void music_unmute()
{
	if (music.IsLoaded())
		music.Unmute();
}

} // namespace devilution
