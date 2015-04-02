#include "config.h"
#include "alMain.h"
#include "alu.h"
#include "threads.h"

#if WINVER >= 0x0602
// Most of this stuff is a hack to workaround the fact that
// xaudio2.h as shipped in the windows 8.1 SDK does not seem to work
// as-is without __cplusplus (perhaps never tested by microsoft)
#define uuid(x)
#define DX_BUILD
#define XAudio2Create XAudio2DontCreate
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")
#undef XAudio2Create
__declspec(dllimport) HRESULT __stdcall
XAudio2Create(_Outptr_ IXAudio2** ppXAudio2, UINT32 Flags X2DEFAULT(0),
	XAUDIO2_PROCESSOR XAudio2Processor X2DEFAULT(XAUDIO2_DEFAULT_PROCESSOR));
#else
#error TODO - This path could be implemented with the June 2010 DirectX SDK
#endif

enum { NUM_BUFFERS = 4 };

static HRESULT CoInitializeResult = E_FAIL;

typedef struct tagXAudio2Data
{
	IXAudio2VoiceCallbackVtbl *lpVtbl;
	IXAudio2 *pXAudio;
	IXAudio2MasteringVoice *pMasteringVoice;
	IXAudio2SourceVoice *pSourceVoice;
	althrd_t thread;
	WAVEFORMATEX Format;
	void *Buffers[NUM_BUFFERS];
	UINT32 BufferIndex, BufferSize;
	HANDLE BufferEndEvent;
	volatile ALboolean killNow;
} XAudio2Data;

static void STDMETHODCALLTYPE OnVoiceProcessingPassStart(IXAudio2VoiceCallback *data, UINT32 BytesRequired) { }
static void STDMETHODCALLTYPE OnVoiceProcessingPassEnd(IXAudio2VoiceCallback *data) { }
static void STDMETHODCALLTYPE OnStreamEnd(IXAudio2VoiceCallback *data) { }
static void STDMETHODCALLTYPE OnBufferStart(IXAudio2VoiceCallback *data, void* pBufferContext) { }
static void STDMETHODCALLTYPE OnBufferEnd(IXAudio2VoiceCallback *data, void* pBufferContext) { SetEvent(((XAudio2Data*)data)->BufferEndEvent); }
static void STDMETHODCALLTYPE OnLoopEnd(IXAudio2VoiceCallback *data, void* pBufferContext) { }
static void STDMETHODCALLTYPE OnVoiceError(IXAudio2VoiceCallback *data, void* pBufferContext, HRESULT Error) { }

static IXAudio2VoiceCallbackVtbl XAudio2DataCallbacks =
{
	OnVoiceProcessingPassStart,
	OnVoiceProcessingPassEnd,
	OnStreamEnd,
	OnBufferStart,
	OnBufferEnd,
	OnLoopEnd,
	OnVoiceError
};

static XAudio2Data *XAudio2Data_New();
static void XAudio2Data_Delete(XAudio2Data *data);
static void XAudio2Data_ResetBuffers(XAudio2Data *data, UINT32 NewSize);
static void XAudio2Data_ReleaseInterfaces(XAudio2Data *data);

static XAudio2Data *XAudio2Data_New()
{
	XAudio2Data *data = (XAudio2Data*)calloc(1, sizeof(XAudio2Data));

	data->lpVtbl = &XAudio2DataCallbacks;
	data->BufferEndEvent = CreateEventEx(NULL, NULL, 0, 0);

	return data;
}

static void XAudio2Data_Delete(XAudio2Data *data)
{
	CloseHandle(data->BufferEndEvent);
	XAudio2Data_ResetBuffers(data, 0);
	XAudio2Data_ReleaseInterfaces(data);
}

static void XAudio2Data_ResetBuffers(XAudio2Data *data, UINT32 NewSize)
{
	if(data->BufferSize != NewSize)
	{
		free(data->Buffers[0]);
		ZeroMemory(data->Buffers, sizeof(data->Buffers));
		if(NewSize)
		{
			data->Buffers[0] = calloc(NUM_BUFFERS, NewSize);
			for(int i = 1; i < NUM_BUFFERS; ++i)
			{
				data->Buffers[i] = (char*)data->Buffers[0] + i*NewSize;
			}
		}
		data->BufferSize = NewSize;
	}
	data->BufferIndex = 0;
}

static void XAudio2Data_ReleaseInterfaces(XAudio2Data *data)
{
	if(data->pSourceVoice)
	{
		IXAudio2SourceVoice_DestroyVoice(data->pSourceVoice);
		data->pSourceVoice = 0;
	}
	if(data->pMasteringVoice)
	{
		IXAudio2MasteringVoice_DestroyVoice(data->pMasteringVoice);
		data->pMasteringVoice = 0;
	}
	if(data->pXAudio)
	{
		IXAudio2_Release(data->pXAudio);
		data->pXAudio = 0;
	}
}

FORCE_ALIGN static int PlaybackThreadProc(void *arg)
{
	ALCdevice *Device = (ALCdevice*)arg;
	XAudio2Data *data = (XAudio2Data*)Device->ExtraData;
	IXAudio2SourceVoice *pSource = data->pSourceVoice;

	SetRTPriority();
	althrd_setname(althrd_current(), MIXER_THREAD_NAME);

	while(!data->killNow)
	{
		XAUDIO2_VOICE_STATE state;
		IXAudio2SourceVoice_GetState(pSource, &state, 0);

		if(state.BuffersQueued >= NUM_BUFFERS)
			WaitForSingleObjectEx(data->BufferEndEvent, INFINITE, FALSE);
		else
		{
			XAUDIO2_BUFFER buf = {0};
			buf.AudioBytes = data->BufferSize;
			buf.pAudioData = (BYTE*)data->Buffers[data->BufferIndex];
			data->BufferIndex = (data->BufferIndex+1)%NUM_BUFFERS;
			aluMixData(Device, (void*)buf.pAudioData, buf.AudioBytes / data->Format.nBlockAlign);
			if(FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(pSource, &buf, NULL)))
				break;
		}		
	}

	IXAudio2SourceVoice_Stop(pSource, 0, XAUDIO2_COMMIT_NOW);
	IXAudio2SourceVoice_FlushSourceBuffers(pSource);

	return S_OK;
}

static ALCenum XAudio2OpenPlayback(ALCdevice *device, const ALCchar *deviceName)
{
	if(0 != (device->Frequency % XAUDIO2_QUANTUM_DENOMINATOR) ||
		device->Frequency < XAUDIO2_MIN_SAMPLE_RATE ||
		device->Frequency > XAUDIO2_MAX_SAMPLE_RATE)
	{
		return ALC_INVALID_VALUE;
	}

	XAudio2Data *data = XAudio2Data_New();
	IXAudio2 *pXAudio = NULL;

	HRESULT hr;

	hr = XAudio2Create(&pXAudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if(FAILED(hr))
		goto failure;
	data->pXAudio = pXAudio;

	if(device->FmtType == DevFmtFloat)
	{
		data->Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		data->Format.wBitsPerSample = 32;
	}
	else
	{
		data->Format.wFormatTag = WAVE_FORMAT_PCM;
		if(device->FmtType == DevFmtUByte || device->FmtType == DevFmtByte)
			data->Format.wBitsPerSample = 8;
		else
			data->Format.wBitsPerSample = 16;
	}
	data->Format.nChannels = ChannelsFromDevFmt(device->FmtChans);
	data->Format.nBlockAlign = data->Format.wBitsPerSample * data->Format.nChannels / 8;
	data->Format.nSamplesPerSec = device->Frequency;
	data->Format.nAvgBytesPerSec = data->Format.nSamplesPerSec * data->Format.nBlockAlign;
	data->Format.cbSize = sizeof(data->Format);

	hr = IXAudio2_CreateMasteringVoice(pXAudio, &data->pMasteringVoice,
		data->Format.nChannels, data->Format.nSamplesPerSec,
		0, NULL, NULL, AudioCategory_GameEffects);
	if(FAILED(hr))
		goto failure;

	hr = IXAudio2_CreateSourceVoice(pXAudio, &data->pSourceVoice,
		&data->Format, 0, 1.0f, (IXAudio2VoiceCallback *)data, NULL, NULL);
	if(FAILED(hr))
		goto failure;
	
	device->ExtraData = data;

	return ALC_NO_ERROR;

failure:

	XAudio2Data_Delete(data);
	device->ExtraData = NULL;

	return ALC_INVALID_VALUE;
}

static void XAudio2ClosePlayback(ALCdevice *device)
{
	XAudio2Data *data = (XAudio2Data*)device->ExtraData;
	XAudio2Data_Delete(data);
	device->ExtraData = NULL;
}

static ALCboolean XAudio2ResetPlayback(ALCdevice *device)
{
	XAudio2Data *data = (XAudio2Data*)device->ExtraData;

	device->UpdateSize = (ALuint)((ALuint64)device->UpdateSize *
		data->Format.nSamplesPerSec / device->Frequency);
	device->UpdateSize = (device->UpdateSize*device->NumUpdates + 3) / 4;
	device->NumUpdates = 4;
	device->Frequency = data->Format.nSamplesPerSec;

	if(data->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
	{
		if(data->Format.wBitsPerSample == 32)
			device->FmtType = DevFmtFloat;
		else
		{
			ERR("Unhandled IEEE float sample depth: %d\n", data->Format.wBitsPerSample);
			return ALC_FALSE;
		}
	}
	else if(data->Format.wFormatTag == WAVE_FORMAT_PCM)
	{
		if(data->Format.wBitsPerSample == 16)
			device->FmtType = DevFmtShort;
		else if(data->Format.wBitsPerSample == 8)
			device->FmtType = DevFmtUByte;
		else
		{
			ERR("Unhandled PCM sample depth: %d\n", data->Format.wBitsPerSample);
			return ALC_FALSE;
		}
	}
	else
	{
		ERR("Unhandled format tag: 0x%04x\n", data->Format.wFormatTag);
		return ALC_FALSE;
	}

	if(data->Format.nChannels == 2)
		device->FmtChans = DevFmtStereo;
	else if(data->Format.nChannels == 1)
		device->FmtChans = DevFmtMono;
	else
	{
		ERR("Unhandled channel count: %d\n", data->Format.nChannels);
		return ALC_FALSE;
	}
	SetDefaultWFXChannelOrder(device);

	return ALC_TRUE;
}

static ALCboolean XAudio2StartPlayback(ALCdevice *device)
{
	XAudio2Data *data = (XAudio2Data*)device->ExtraData;

	UINT32 BufferSize;

	BufferSize  = device->UpdateSize*device->NumUpdates / NUM_BUFFERS;
	BufferSize *= FrameSizeFromDevFmt(device->FmtChans, device->FmtType);
	XAudio2Data_ResetBuffers(data, BufferSize);

	HRESULT hr;

	hr = IXAudio2_StartEngine(data->pXAudio);
	if(FAILED(hr))
		return ALC_FALSE;
	
	hr = IXAudio2SourceVoice_Start(data->pSourceVoice, 0, XAUDIO2_COMMIT_NOW);
	if(FAILED(hr))
		return ALC_FALSE;

	data->killNow = AL_FALSE;
	if(althrd_create(&data->thread, PlaybackThreadProc, device) != althrd_success)
		return ALC_FALSE;

	return ALC_TRUE;
}

static void XAudio2StopPlayback(ALCdevice *device)
{
	XAudio2Data *data = (XAudio2Data*)device->ExtraData;

	if(data->killNow)
		return;

	// Set flag to stop processing headers
	data->killNow = AL_TRUE;
	althrd_join(data->thread, NULL);

	IXAudio2_StopEngine(data->pXAudio);

	XAudio2Data_ResetBuffers(data, 0);
}

static const BackendFuncs XAudio2Funcs = {
	XAudio2OpenPlayback,
	XAudio2ClosePlayback,
	XAudio2ResetPlayback,
	XAudio2StartPlayback,
	XAudio2StopPlayback,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

ALCboolean alc_xaudio2_init(BackendFuncs *FuncList)
{
	CoInitializeResult = CoInitializeEx(NULL, 0);
	*FuncList = XAudio2Funcs;
	return ALC_TRUE;
}

void alc_xaudio2_deinit(void)
{
	if(SUCCEEDED(CoInitializeResult))
	{
		CoUninitialize();
		CoInitializeResult = E_FAIL;
	}
}

void alc_xaudio2_probe(enum DevProbe type)
{
	switch(type)
	{
		case ALL_DEVICE_PROBE:
			AppendAllDevicesList("XAudio2 Default Device");
			break;
		case CAPTURE_DEVICE_PROBE:
			break;
	}
}
