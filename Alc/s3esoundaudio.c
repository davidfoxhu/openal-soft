
#include "openal_config.h"

#include <malloc.h>
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"
#include "alu.h"
#include <s3eSound.h>
#include <s3eThread.h>
#include <s3eDevice.h>
#include <s3eDebug.h>
#include <s3eTimer.h>
#include <threads.h>

static const ALCchar s3eDevice[] = "s3eSound";

typedef struct _s3eSoundData
{
    int channel;

    s3eThreadSem* pGetAudioSem;
    s3eThreadSem* pGetAudioSemDone;
    s3eThread* pGetAudioThreadHandler;
    s3eSoundGenAudioInfo* pAudioInfo;
    bool bStopGetAudioThread;

    ALCdevice* pDevice;

    void* pDataBuffer;
    int nDataLength;
}s3eSoundData;

static int32 s3eAudioRequest(void* systemData, void* userData)
{
    int nOldValue = 0;
    int nValue = 0;
    s3eSoundData* pData = (s3eSoundData*)userData;
    pData->pAudioInfo = (s3eSoundGenAudioInfo*)systemData;

    s3eThreadSemGetValue(pData->pGetAudioSemDone, &nOldValue);
    s3eThreadSemPost(pData->pGetAudioSem);
    nValue = nOldValue;
    while (nValue == nOldValue)
    {
        s3eThreadSemGetValue(pData->pGetAudioSemDone, &nValue);
        sleep(0);
    }

    return pData->pAudioInfo->m_NumSamples;
}

static void* s3eGetAudioThread(void* userData)
{
    s3eSoundData* pData = (s3eSoundData*)userData;
    while(!pData->bStopGetAudioThread)
    {
        s3eThreadSemWait(pData->pGetAudioSem, -1);
        if (pData->bStopGetAudioThread)
            return NULL;

        aluMixData(pData->pDevice, pData->pAudioInfo->m_Target, pData->pAudioInfo->m_NumSamples);
        s3eThreadSemPost(pData->pGetAudioSemDone);
    }
    return NULL;
}

static ALCenum s3e_open_playback(ALCdevice *device, const ALCchar *deviceName)
{
    s3eSoundData* pData = 0;

    if (!deviceName)
        deviceName = s3eDevice;

    if (strcmp(s3eDevice, deviceName) != 0)
        return ALC_FALSE;

    al_string_append_cstr(&device->DeviceName, deviceName);

    pData = (s3eSoundData*)malloc(sizeof(s3eSoundData));
    memset(pData, 0, sizeof(s3eSoundData));

    pData->pDevice = device;

    pData->channel = s3eSoundGetFreeChannel();
    if(pData->channel == -1)
    {
        free(pData);
        return ALC_FALSE;   // Could not set up the channel
    }

    device->ExtraData = pData;

    device->FmtType = DevFmtShort;  // 16 bit per channel
    // when generating sound, channel frequency is ignored - we use output device frequency
    device->Frequency = s3eSoundGetInt(S3E_SOUND_OUTPUT_FREQ);

    if(s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED) == S3E_TRUE)
        device->FmtChans = DevFmtStereo;
    else
        device->FmtChans = DevFmtMono;

    return ALC_NO_ERROR;
}

static void s3e_close_playback(ALCdevice *device)
{
    s3eSoundData* pData = (s3eSoundData*)device->ExtraData;
    device->ExtraData = NULL;
    free(pData);
}

static ALCboolean s3e_reset_playback( ALCdevice *device )
{
    int nRet = ALC_FALSE;
    s3eSoundData* pData = (s3eSoundData*)device->ExtraData;
    //assert( device->UpdateSize == device->Frequency / 50 );

    int sampleSize = device->UpdateSize;
    int bytesPerSample = FrameSizeFromDevFmt(device->FmtChans, device->FmtType) * sizeof(ALubyte);
    assert( bytesPerSample == (s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED) == S3E_TRUE ? 4 : 2) );

    pData->pDataBuffer = (ALubyte*)calloc(1, sampleSize * bytesPerSample);
    pData->nDataLength = sampleSize * bytesPerSample;
    if(!pData->pDataBuffer)
    {
        AL_PRINT("buffer malloc failed\n");
        return ALC_FALSE;
    }
    SetDefaultWFXChannelOrder(device);

    pData->pGetAudioSem = s3eThreadSemCreate(0);
    pData->pGetAudioSemDone = s3eThreadSemCreate(0);
    pData->pAudioInfo = NULL;
    pData->bStopGetAudioThread = false;
    pData->pGetAudioThreadHandler = s3eThreadCreate(s3eGetAudioThread, pData, 0, 0, 0);

    // Register callback functions
    s3eSoundChannelRegister(pData->channel, S3E_CHANNEL_GEN_AUDIO, s3eAudioRequest, pData);

    // Check if we have stereo sound
    if(s3eSoundGetInt(S3E_SOUND_STEREO_ENABLED) == S3E_TRUE)
        s3eSoundChannelRegister(pData->channel, S3E_CHANNEL_GEN_AUDIO_STEREO, s3eAudioRequest, pData);

    // Starting infinite playback cycle with any data
    s3eSoundChannelPlay(pData->channel, (int16*)pData->pDataBuffer, sampleSize * bytesPerSample / 2, 0, 0);
    return ALC_TRUE;
}

static void s3e_stop_playback( ALCdevice *device )
{
    s3eSoundData* pData = (s3eSoundData*)device->ExtraData;

    // Stop the callback functions
    s3eSoundChannelStop(pData->channel);
    s3eSoundChannelUnRegister(pData->channel, S3E_CHANNEL_GEN_AUDIO_STEREO);
    s3eSoundChannelUnRegister(pData->channel, S3E_CHANNEL_GEN_AUDIO);

    if( pData->pDataBuffer != NULL )
    {
        memset(pData->pDataBuffer, 0, pData->nDataLength);
        free(pData->pDataBuffer);
        pData->pDataBuffer = NULL;
    }

    pData->bStopGetAudioThread = true;
    s3eThreadSemPost(pData->pGetAudioSem);
    s3eThreadJoin(pData->pGetAudioThreadHandler, NULL);
    pData->pGetAudioThreadHandler = 0;

    s3eThreadSemDestroy(pData->pGetAudioSem);
    pData->pGetAudioSem = 0;
    s3eThreadSemDestroy(pData->pGetAudioSemDone);
    pData->pGetAudioSemDone = 0;

    pData->pAudioInfo = NULL;
}

static ALCenum s3e_open_capture( ALCdevice *pDevice, const ALCchar* pName)
{
    // maybe one day
    (void)pDevice;
    return ALC_INVALID_DEVICE;
}


ALCboolean s3e_start_playback(ALCdevice* device)
{
    return ALC_TRUE;
}

BackendFuncs s3e_funcs = {
    s3e_open_playback,
    s3e_close_playback,
    s3e_reset_playback,
    s3e_start_playback,
    s3e_stop_playback,
    s3e_open_capture,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

ALCboolean alc_s3e_init(BackendFuncs *func_list)
{
    *func_list = s3e_funcs;
    return 1;
}

void alc_s3e_deinit(void)
{
}

void alc_s3e_probe(enum DevProbe type)
{
    if(type == ALL_DEVICE_PROBE)
        AppendAllDevicesList(s3eDevice);
}

