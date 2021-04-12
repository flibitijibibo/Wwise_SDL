/*******************************************************************************
The content of this file includes portions of the AUDIOKINETIC Wwise Technology
released in source code form as part of the SDK installer package.

Commercial License Usage

Licensees holding valid commercial licenses to the AUDIOKINETIC Wwise Technology
may use this file in accordance with the end user license agreement provided
with the software or, alternatively, in accordance with the terms contained in a
written agreement between you and Audiokinetic Inc.

Apache License Usage

Alternatively, this file may be used under the Apache License, Version 2.0 (the
"Apache License"); you may not use this file except in compliance with the
Apache License. You may obtain a copy of the Apache License at
http://www.apache.org/licenses/LICENSE-2.0.

Unless required by applicable law or agreed to in writing, software distributed
under the Apache License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES
OR CONDITIONS OF ANY KIND, either express or implied. See the Apache License for
the specific language governing permissions and limitations under the License.

  Copyright (c) 2021 Audiokinetic Inc.
*******************************************************************************/

#include <AK/AkWwiseSDKVersion.h>
#include <AK/SoundEngine/Common/IAkPlugin.h>
#include <AK/Plugin/PluginServices/AkFXParameterChangeHandler.h>
#include <AK/Tools/Common/AkBankReadHelpers.h>

#if AK_WWISESDK_VERSION_COMBINED > ((2019 << 8) | 1)
#include <AK/SoundEngine/Common/AkAtomic.h>
#else
static inline int32_t AkAtomicLoad32(AkAtomic32 *pSrc)
{
	return *pSrc;
}
static inline int32_t AkAtomicAdd32(AkAtomic32 *pDest, int32_t value)
{
	return __sync_add_and_fetch((int32_t*) pDest, value);
}
static inline int32_t AkAtomicSub32(AkAtomic32 *pDest, int32_t value)
{
	return __sync_sub_and_fetch((int32_t*) pDest, value);
}
#endif /* > 2019.1 */

#include <SDL.h>

class SDL2OutputSink
	: public AK::IAkSinkPlugin
{
public:
	SDL2OutputSink();
	~SDL2OutputSink();

	AKRESULT Init(
		AK::IAkPluginMemAlloc *in_pAllocator,
		AK::IAkSinkPluginContext *in_pContext,
		AK::IAkPluginParam *in_pParams,
		AkAudioFormat& io_rFormat
	) override;
	AKRESULT Term(AK::IAkPluginMemAlloc *in_pAllocator) override;
	AKRESULT Reset() override;
	AKRESULT GetPluginInfo(AkPluginInfo& out_rPluginInfo) override;
	AKRESULT IsDataNeeded(AkUInt32& out_uNumFramesNeeded) override;
	void Consume(AkAudioBuffer *in_pInputBuffer, AkRamp in_gain) override;
	void OnFrameEnd() override;
	bool IsStarved() override;
	void ResetStarved() override;

	void AudioCallback(float *stream, int numsamples);

private:
	AKRESULT AllocBuffer(AkUInt32 in_size);
	void DestroyBuffer();

	AkForceInline void* GetRefillPosition()
	{
		return (float*) m_pData + (m_WriteHead * m_SpeakersConfig.uNumChannels);
	}

	AK::IAkPluginMemAlloc *m_pAllocator = nullptr;
	AK::IAkSinkPluginContext *m_pContext = nullptr;
	bool m_bStarved = false;
	bool m_bDataReady = false;

	bool m_bRun = false;
	AkUInt32 m_uDeviceId = 0;

	AkChannelConfig m_SpeakersConfig;
	AkUInt32 m_ReadHead = 0;
	AkUInt32 m_BufferSize = 0;
	AkInt32 m_WriteHead = 0;
	AkAtomic32 m_SamplesReady = 0;
	void* m_pData = nullptr;
};

static void SDLCALL SDLAudioCallback(void* userdata, Uint8 *stream, int len)
{
	((SDL2OutputSink*) userdata)->AudioCallback(
		(float*) stream,
		len / sizeof(float)
	);
}

SDL2OutputSink::SDL2OutputSink()
{
}

SDL2OutputSink::~SDL2OutputSink()
{
	DestroyBuffer();
}

AKRESULT SDL2OutputSink::Init(
	AK::IAkPluginMemAlloc *in_pAllocator,
	AK::IAkSinkPluginContext *in_pCtx,
	AK::IAkPluginParam *in_pParams,
	AkAudioFormat& io_rFormat
) {
	m_pAllocator = in_pAllocator;
	m_pContext = in_pCtx;

	if (!SDL_WasInit(SDL_INIT_AUDIO))
	{
		if (SDL_Init(SDL_INIT_AUDIO) == -1)
		{
			return AK_Fail;
		}
	}

	m_SpeakersConfig = io_rFormat.channelConfig;

	SDL_AudioSpec desired;
	SDL_zero(desired);

	desired.freq = m_pContext->GlobalContext()->GetSampleRate();
	desired.format = AUDIO_F32SYS;
	desired.samples = m_pContext->GlobalContext()->GetMaxBufferLength();
	desired.callback = SDLAudioCallback;
	desired.userdata = this;

	if (!m_SpeakersConfig.IsValid())
	{
		/* Okay, so go grab something from the liquor cabinet and get
		 * ready, because this loop is a bit of a trip:
		 *
		 * We can't get the spec for the default device, because in
		 * audio land a "default device" is a completely foreign idea,
		 * some APIs support it but in reality you just have to pass
		 * NULL as a driver string and the sound server figures out the
		 * rest. In some psychotic universe the device can even be a
		 * network address. No, seriously.
		 *
		 * So what do we do? Well, at least in my experience shipping
		 * for the PC, the easiest thing to do is assume that the
		 * highest spec in the list is what you should target, even if
		 * it turns out that's not the default at the time you create
		 * your device.
		 *
		 * Consider a laptop that has built-in stereo speakers, but is
		 * connected to a home theater system with 5.1 audio. It may be
		 * the case that the stereo audio is active, but the user may
		 * at some point move audio to 5.1, at which point the server
		 * will simply move the endpoint from underneath us and move our
		 * output stream to the new device. At that point, you _really_
		 * want to already be pushing out 5.1, because if not the user
		 * will be stuck recreating the whole program, which on many
		 * platforms is an instant cert failure. The tradeoff is that
		 * you're potentially downmixing a 5.1 stream to stereo, which
		 * is a bit wasteful, but presumably the hardware can handle it
		 * if they were able to use a 5.1 system to begin with.
		 *
		 * So, we just aim for the highest channel count on the system.
		 * We also do this with sample rate to a lesser degree; we try
		 * to use a single device spec at a time, so it may be that
		 * the sample rate you get isn't the highest from the list if
		 * another device had a higher channel count.
		 *
		 * Lastly, if you set SDL_AUDIO_CHANNELS but not
		 * SDL_AUDIO_FREQUENCY, we don't bother checking for a sample
		 * rate, we fall through to the hardcoded value at the bottom of
		 * this function.
		 *
		 * I'm so tired.
		 *
		 * -flibit
		 */
		SDL_AudioSpec spec;
		int channels;
		const char *envvar;
		envvar = SDL_getenv("SDL_AUDIO_CHANNELS");
		if (envvar != NULL)
		{
			channels = SDL_atoi(envvar);
		}
		else
		{
			channels = 0;
		}
#if SDL_VERSION_ATLEAST(2, 0, 15)
		if (channels <= 0)
		{
			int devcount = SDL_GetNumAudioDevices(0);
			for (int i = 0; i < devcount; i += 1)
			{
				SDL_GetAudioDeviceSpec(i, 0, &spec);
				if (spec.channels > channels)
				{
					channels = spec.channels;
				}
			}
		}
#endif /* SDL >= 2.0.15 */
		if (channels <= 0)
		{
			channels = 2;
		}

		switch (channels)
		{
		case 8:
			m_SpeakersConfig.SetStandard(AK_SPEAKER_SETUP_7POINT1);
			break;
		case 6:
			m_SpeakersConfig.SetStandard(AK_SPEAKER_SETUP_5POINT1);
			break;
		case 4:
			m_SpeakersConfig.SetStandard(AK_SPEAKER_SETUP_4);
			break;
		case 2:
			m_SpeakersConfig.SetStandard(AK_SPEAKER_SETUP_STEREO);
			break;
		default:
			SDL_Log("CAkSinkSDL2::Init(): Unknown channel configuration: %d\n", channels);
			return AK_Fail;
		}
	}

	desired.channels = m_SpeakersConfig.uNumChannels;
	m_uDeviceId = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
	if (m_uDeviceId == 0)
	{
		SDL_Log("CAkSinkSDL2::Init(): %s\n", SDL_GetError());
		return AK_Fail;
	}

	io_rFormat.channelConfig = m_SpeakersConfig;
	AllocBuffer(m_pContext->GetNumRefillsInVoice() * m_pContext->GlobalContext()->GetMaxBufferLength());

	if (m_pData == NULL)
	{
		return AK_InsufficientMemory;
	}

	return AK_Success;
}

AKRESULT SDL2OutputSink::Term(AK::IAkPluginMemAlloc *in_pAllocator)
{
	m_bRun = false;

	if (m_uDeviceId)
	{
		SDL_PauseAudioDevice(m_uDeviceId, 1);
		SDL_CloseAudioDevice(m_uDeviceId);
		m_uDeviceId = 0;
	}

	AK_PLUGIN_DELETE(in_pAllocator, this);
	return AK_Success;
}

AKRESULT SDL2OutputSink::Reset()
{
	m_bRun = true;
	SDL_PauseAudioDevice(m_uDeviceId, 0);
	return AK_Success;
}

AKRESULT SDL2OutputSink::GetPluginInfo(AkPluginInfo& out_rPluginInfo)
{
	out_rPluginInfo.eType = AkPluginTypeSink;
	out_rPluginInfo.bIsInPlace = true;
	out_rPluginInfo.uBuildVersion = AK_WWISESDK_VERSION_COMBINED;
	return AK_Success;
}

AKRESULT SDL2OutputSink::IsDataNeeded(AkUInt32& out_uNumFramesNeeded)
{
	AkUInt32 samplesReady = AkAtomicLoad32(&m_SamplesReady);
	AkUInt32 uNumFrames = m_pContext->GlobalContext()->GetMaxBufferLength();
	out_uNumFramesNeeded = (m_BufferSize - AkMin(m_BufferSize, samplesReady)) / uNumFrames;

	return AK_Success;
}

void SDL2OutputSink::Consume(AkAudioBuffer *in_pInputBuffer, AkRamp in_gain)
{
	if (in_pInputBuffer->uValidFrames > 0)
	{
		AkUInt32 uNumFrames = in_pInputBuffer->MaxFrames();
		AkUInt32 uNumChannels = in_pInputBuffer->NumChannels();
		const AkReal32 fGainStart = in_gain.fPrev;
		const AkReal32 fDelta = (in_gain.fNext - in_gain.fPrev) / (AkReal32) uNumFrames;

		AkReal32 *pRefill = (AkReal32*) GetRefillPosition();

		/* Interleave and apply gain ramp */
		for (AkUInt32 iChannel = 0; iChannel < uNumChannels; ++iChannel)
		{
			AkReal32 *AK_RESTRICT pSrc = in_pInputBuffer->GetChannel(iChannel);
			AkReal32 *AK_RESTRICT pDst = pRefill + iChannel;
			AkReal32 fGain = fGainStart;
			for (unsigned int i = 0; i < uNumFrames; i++)
			{
				*pDst = *pSrc++ * fGain;
				pDst += uNumChannels;
				fGain += fDelta;
			}
		}

		AkAtomicAdd32(&m_SamplesReady, uNumFrames);
		m_WriteHead = (m_WriteHead + uNumFrames) % m_BufferSize;

		/* Consume input buffer and send it to the output here */
		m_bDataReady = true;
	}
}

void SDL2OutputSink::OnFrameEnd()
{
	if (!m_bDataReady)
	{
		/* Consume was not called for this audio frame, send silence to the output here */
		AkUInt32 uNumFrames = m_pContext->GlobalContext()->GetMaxBufferLength();

		SDL_memset(
			GetRefillPosition(),
			'\0',
			uNumFrames * m_SpeakersConfig.uNumChannels * sizeof(AkReal32)
		);

		AkAtomicAdd32(&m_SamplesReady, uNumFrames);
		m_WriteHead = (m_WriteHead + uNumFrames) % m_BufferSize;
	}

	m_bDataReady = false;
}

bool SDL2OutputSink::IsStarved()
{
	return m_bStarved;
}

void SDL2OutputSink::ResetStarved()
{
	m_bStarved = false;
}

AKRESULT SDL2OutputSink::AllocBuffer(AkUInt32 in_size)
{
	DestroyBuffer();

	AKASSERT(in_size % m_pContext->GlobalContext()->GetMaxBufferLength() == 0);

	AkUInt32 numChannels = m_SpeakersConfig.uNumChannels;
	size_t sizeInBytes = sizeof(AkReal32) * in_size * numChannels;

	m_pData = AK_PLUGIN_ALLOC(m_pAllocator, sizeInBytes);
	if (m_pData)
	{
		/* The buffer starts full with silence. */
		SDL_memset(m_pData, '\0', sizeInBytes);
		m_BufferSize = in_size;
		m_SamplesReady = in_size;
		m_ReadHead = 0;

		return AK_Success;
	}
	else
	{
		return AK_Fail;
	}
}

void SDL2OutputSink::DestroyBuffer()
{
	if (m_pData)
	{
		AK_PLUGIN_FREE(m_pAllocator, m_pData);
		m_pData = NULL;
	}

	m_BufferSize = 0;
	m_WriteHead = m_ReadHead = m_SamplesReady = 0;
}

void SDL2OutputSink::AudioCallback(float *stream, int numsamples)
{
	if (m_bRun)
	{
		const AkUInt32 numChannels = m_SpeakersConfig.uNumChannels;
		while ((m_SamplesReady > 0) && (numsamples > 0))
		{
			const float *src = ((float*) m_pData) + m_ReadHead * numChannels;
			AkUInt32 samplesReady = AkAtomicLoad32(&m_SamplesReady);
			AkUInt32 framesAvail = AkMin(samplesReady, m_BufferSize - m_ReadHead);
			AkUInt32 samplesToCopy = AkMin(framesAvail * numChannels, (AkUInt32) numsamples);

			SDL_memcpy(stream, src, samplesToCopy * sizeof(float));
			stream += samplesToCopy;
			numsamples -= samplesToCopy;

			int framesCopied = samplesToCopy / numChannels;
			AkAtomicSub32(&m_SamplesReady, framesCopied);
			m_ReadHead += framesCopied;
			m_ReadHead %= m_BufferSize;
		}

		if (m_bRun)
		{
			m_pContext->SignalAudioThread();
		}
	}

	if (numsamples > 0)
	{
		m_bStarved = true;
		SDL_memset(stream, '\0', numsamples * sizeof(float));
	}
}

#ifndef DISABLE_EXTREMELY_GOOD_LINKER_HACK

AK::IAkPlugin* AkCreateDefaultSink(AK::IAkPluginMemAlloc* in_pAllocator)
{
	return AK_PLUGIN_NEW(in_pAllocator, SDL2OutputSink());
}

#else

/* There's supposed to be a whole struct here, but this is for authoring only */

struct SDL2OutputSinkParams
	: public AK::IAkPluginParam
{
	SDL2OutputSinkParams() { }
	SDL2OutputSinkParams(const SDL2OutputSinkParams& in_rParams) { }
	~SDL2OutputSinkParams() { }
	IAkPluginParam* Clone(AK::IAkPluginMemAlloc* in_pAllocator) override { }
	AKRESULT Init(
		AK::IAkPluginMemAlloc* in_pAllocator,
		const void* in_pParamsBlock,
		AkUInt32 in_ulBlockSize
	) override {
		return AK_Success;
	};
	AKRESULT Term(AK::IAkPluginMemAlloc* in_pAllocator) override
	{
		return AK_Success;
	};
	AKRESULT SetParamsBlock(
		const void* in_pParamsBlock,
		AkUInt32 in_ulBlockSize
	) override {
		return AK_Success;
	};
	AKRESULT SetParam(
		AkPluginParamID in_paramID,
		const void* in_pValue,
		AkUInt32 in_ulParamSize
	) override {
		return AK_Success;
	}
};

/* This is what we _should_ be doing, but alas... */

AK::IAkPlugin* CreateSDL2OutputSink(AK::IAkPluginMemAlloc* in_pAllocator)
{
	return AK_PLUGIN_NEW(in_pAllocator, SDL2OutputSink());
}

AK::IAkPluginParam* CreateSDL2OutputSinkParams(AK::IAkPluginMemAlloc* in_pAllocator)
{
	return AK_PLUGIN_NEW(in_pAllocator, SDL2OutputSinkParams());
}

AK_IMPLEMENT_PLUGIN_FACTORY(
	SDL2OutputSink,
	AkPluginTypeSink,
	SDL2OutputConfig::CompanyID,
	SDL2OutputConfig::PluginID
)

AK_STATIC_LINK_PLUGIN(SDL2OutputSink)

#endif /* DISABLE_EXTREMELY_GOOD_LINKER_HACK */
