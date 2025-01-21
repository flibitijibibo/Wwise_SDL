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

#include <SDL3/SDL.h>

class SDL3OutputSink
	: public AK::IAkSinkPlugin
{
public:
	SDL3OutputSink();
	~SDL3OutputSink();

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
	SDL_AudioStream *m_pDeviceId = nullptr;

	AkChannelConfig m_SpeakersConfig;
	AkUInt32 m_ReadHead = 0;
	AkUInt32 m_BufferSize = 0;
	AkInt32 m_WriteHead = 0;
	AkAtomic32 m_SamplesReady = 0;
	void* m_pData = nullptr;

	static void SDLCALL SDLAudioCallback(
		void *userdata,
		SDL_AudioStream *stream,
		int additional_amount,
		int total_amount
	);
};

void SDLCALL SDL3OutputSink::SDLAudioCallback(
	void *userdata,
	SDL_AudioStream *stream,
	int additional_amount,
	int total_amount
) {
	SDL3OutputSink *pThis = (SDL3OutputSink*) userdata;

	int additional_samples = (
		additional_amount /
		sizeof(float) /
		pThis->m_SpeakersConfig.uNumChannels
	);
	while (additional_samples > 0)
	{
		int samples = SDL_min(
			additional_samples,
			pThis->m_pContext->GlobalContext()->GetMaxBufferLength()
		);
		float staging[samples * pThis->m_SpeakersConfig.uNumChannels];

		pThis->AudioCallback(staging, samples * pThis->m_SpeakersConfig.uNumChannels);

		SDL_PutAudioStreamData(
			stream,
			staging,
			sizeof(staging)
		);

		additional_samples -= samples;
	}
}

SDL3OutputSink::SDL3OutputSink()
{
}

SDL3OutputSink::~SDL3OutputSink()
{
	DestroyBuffer();
}

AKRESULT SDL3OutputSink::Init(
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
	desired.format = SDL_AUDIO_F32;

	if (!m_SpeakersConfig.IsValid())
	{
		int channels;
		const char *envvar;
		envvar = SDL_getenv("SDL_AUDIO_CHANNELS");
		if (envvar != NULL)
		{
			channels = SDL_atoi(envvar);
		}
		else
		{
			SDL_AudioSpec spec;
			SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL);
			channels = spec.channels;
		}

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
			SDL_Log("CAkSinkSDL3::Init(): Unknown channel configuration: %d\n", channels);
			return AK_Fail;
		}
	}

	desired.channels = m_SpeakersConfig.uNumChannels;
	m_pDeviceId = SDL_OpenAudioDeviceStream(
		SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&desired,
		SDLAudioCallback,
		this
	);
	if (m_pDeviceId == nullptr)
	{
		SDL_Log("CAkSinkSDL3::Init(): %s\n", SDL_GetError());
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

AKRESULT SDL3OutputSink::Term(AK::IAkPluginMemAlloc *in_pAllocator)
{
	m_bRun = false;

	if (m_pDeviceId != nullptr)
	{
		SDL_PauseAudioStreamDevice(m_pDeviceId);
		SDL_DestroyAudioStream(m_pDeviceId);
		m_pDeviceId = NULL;
	}

	AK_PLUGIN_DELETE(in_pAllocator, this);
	return AK_Success;
}

AKRESULT SDL3OutputSink::Reset()
{
	m_bRun = true;
	SDL_ResumeAudioStreamDevice(m_pDeviceId);
	return AK_Success;
}

AKRESULT SDL3OutputSink::GetPluginInfo(AkPluginInfo& out_rPluginInfo)
{
	out_rPluginInfo.eType = AkPluginTypeSink;
	out_rPluginInfo.bIsInPlace = true;
	out_rPluginInfo.uBuildVersion = AK_WWISESDK_VERSION_COMBINED;
	return AK_Success;
}

AKRESULT SDL3OutputSink::IsDataNeeded(AkUInt32& out_uNumFramesNeeded)
{
	AkUInt32 samplesReady = AkAtomicLoad32(&m_SamplesReady);
	AkUInt32 uNumFrames = m_pContext->GlobalContext()->GetMaxBufferLength();
	out_uNumFramesNeeded = (m_BufferSize - AkMin(m_BufferSize, samplesReady)) / uNumFrames;

	return AK_Success;
}

void SDL3OutputSink::Consume(AkAudioBuffer *in_pInputBuffer, AkRamp in_gain)
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

void SDL3OutputSink::OnFrameEnd()
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

bool SDL3OutputSink::IsStarved()
{
	return m_bStarved;
}

void SDL3OutputSink::ResetStarved()
{
	m_bStarved = false;
}

AKRESULT SDL3OutputSink::AllocBuffer(AkUInt32 in_size)
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

void SDL3OutputSink::DestroyBuffer()
{
	if (m_pData)
	{
		AK_PLUGIN_FREE(m_pAllocator, m_pData);
		m_pData = NULL;
	}

	m_BufferSize = 0;
	m_WriteHead = m_ReadHead = m_SamplesReady = 0;
}

void SDL3OutputSink::AudioCallback(float *stream, int numsamples)
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
	return AK_PLUGIN_NEW(in_pAllocator, SDL3OutputSink());
}

#else

/* There's supposed to be a whole struct here, but this is for authoring only */

struct SDL3OutputSinkParams
	: public AK::IAkPluginParam
{
	SDL3OutputSinkParams() { }
	SDL3OutputSinkParams(const SDL3OutputSinkParams& in_rParams) { }
	~SDL3OutputSinkParams() { }
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

AK::IAkPlugin* CreateSDL3OutputSink(AK::IAkPluginMemAlloc* in_pAllocator)
{
	return AK_PLUGIN_NEW(in_pAllocator, SDL3OutputSink());
}

AK::IAkPluginParam* CreateSDL3OutputSinkParams(AK::IAkPluginMemAlloc* in_pAllocator)
{
	return AK_PLUGIN_NEW(in_pAllocator, SDL3OutputSinkParams());
}

AK_IMPLEMENT_PLUGIN_FACTORY(
	SDL3OutputSink,
	AkPluginTypeSink,
	SDL3OutputConfig::CompanyID,
	SDL3OutputConfig::PluginID
)

AK_STATIC_LINK_PLUGIN(SDL3OutputSink)

#endif /* DISABLE_EXTREMELY_GOOD_LINKER_HACK */
