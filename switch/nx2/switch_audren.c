// switch_audren: audio output through the audio renderer (audren) for the NX ports. See switch_audren.h.
// SPDX-License-Identifier: MIT
#if defined(__SWITCH__)

#include "switch_audren.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define SWITCH_AUDREN_MAX_BUFFERS 16
#define SWITCH_AUDREN_VOICE 0

struct SwitchAudren
{
	AudioDriver driver;
	Mutex mutex;
	void* pool;
	size_t pool_size;
	uint32_t frames_per_buffer;
	uint32_t buffer_count;
	uint32_t next;
	bool started;
	bool paused;
	AudioDriverWaveBuf wavebufs[SWITCH_AUDREN_MAX_BUFFERS];
};

static const AudioRendererConfig s_config = {
	.output_rate = AudioRendererOutputRate_48kHz,
	.num_voices = 1,
	.num_effects = 0,
	.num_sinks = 1,
	.num_mix_objs = 1,
	.num_mix_buffers = 2,
};

SwitchAudren* switchAudrenOpen(uint32_t sample_rate, uint32_t frames_per_buffer, uint32_t buffer_count,
							   uint32_t* result)
{
	if (result)
		*result = 0;
	if (!sample_rate || !frames_per_buffer || buffer_count < 2 || buffer_count > SWITCH_AUDREN_MAX_BUFFERS)
	{
		if (result)
			*result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
		return NULL;
	}
	SwitchAudren* audren = (SwitchAudren*)calloc(1, sizeof(SwitchAudren));
	if (!audren)
		return NULL;
	mutexInit(&audren->mutex);
	audren->frames_per_buffer = frames_per_buffer;
	audren->buffer_count = buffer_count;
	const size_t buffer_bytes = (size_t)frames_per_buffer * 2 * sizeof(int16_t);
	audren->pool_size = (buffer_bytes * buffer_count + 0xFFF) & ~(size_t)0xFFF;
	audren->pool = memalign(0x1000, audren->pool_size);
	if (!audren->pool)
	{
		free(audren);
		if (result)
			*result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
		return NULL;
	}
	memset(audren->pool, 0, audren->pool_size);

	Result rc = audrenInitialize(&s_config);
	if (R_SUCCEEDED(rc))
	{
		rc = audrvCreate(&audren->driver, &s_config, 2);
		if (R_FAILED(rc))
			audrenExit();
	}
	if (R_FAILED(rc))
	{
		free(audren->pool);
		free(audren);
		if (result)
			*result = rc;
		return NULL;
	}

	static const u8 sink_channels[] = {0, 1};
	const int pool_id = audrvMemPoolAdd(&audren->driver, audren->pool, audren->pool_size);
	const bool ok = pool_id >= 0 && audrvMemPoolAttach(&audren->driver, pool_id) &&
					audrvDeviceSinkAdd(&audren->driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels) >= 0 &&
					audrvVoiceInit(&audren->driver, SWITCH_AUDREN_VOICE, 2, PcmFormat_Int16, (int)sample_rate);
	if (ok)
	{
		audrvVoiceSetDestinationMix(&audren->driver, SWITCH_AUDREN_VOICE, AUDREN_FINAL_MIX_ID);
		audrvVoiceSetMixFactor(&audren->driver, SWITCH_AUDREN_VOICE, 1.0f, 0, 0);
		audrvVoiceSetMixFactor(&audren->driver, SWITCH_AUDREN_VOICE, 0.0f, 0, 1);
		audrvVoiceSetMixFactor(&audren->driver, SWITCH_AUDREN_VOICE, 0.0f, 1, 0);
		audrvVoiceSetMixFactor(&audren->driver, SWITCH_AUDREN_VOICE, 1.0f, 1, 1);
		rc = audrvUpdate(&audren->driver);
		if (R_SUCCEEDED(rc))
			rc = audrenStartAudioRenderer();
	}
	else
		rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
	if (R_FAILED(rc))
	{
		audrvClose(&audren->driver);
		audrenExit();
		free(audren->pool);
		free(audren);
		if (result)
			*result = rc;
		return NULL;
	}

	for (uint32_t i = 0; i < buffer_count; i++)
	{
		AudioDriverWaveBuf* wavebuf = &audren->wavebufs[i];
		wavebuf->data_raw = (const u8*)audren->pool + buffer_bytes * i;
		wavebuf->size = buffer_bytes;
		wavebuf->start_sample_offset = 0;
		wavebuf->end_sample_offset = (s32)frames_per_buffer;
	}
	return audren;
}

static bool switchAudrenBufferFree(const AudioDriverWaveBuf* wavebuf)
{
	return wavebuf->state == AudioDriverWaveBufState_Free || wavebuf->state == AudioDriverWaveBufState_Done;
}

int16_t* switchAudrenAcquire(SwitchAudren* audren, uint64_t timeout_ns)
{
	AudioDriverWaveBuf* wavebuf = &audren->wavebufs[audren->next];
	const uint64_t start = armTicksToNs(armGetSystemTick());
	for (;;)
	{
		mutexLock(&audren->mutex);
		const bool free_now = switchAudrenBufferFree(wavebuf);
		mutexUnlock(&audren->mutex);
		if (free_now)
			return wavebuf->data_pcm16;
		if (armTicksToNs(armGetSystemTick()) - start >= timeout_ns)
			return NULL;
		// (about 5 ms, one audio renderer frame; the update reads which buffers the DSP has played)
		audrenWaitFrame();
		mutexLock(&audren->mutex);
		audrvUpdate(&audren->driver);
		mutexUnlock(&audren->mutex);
	}
}

void switchAudrenSubmit(SwitchAudren* audren, int16_t* samples)
{
	AudioDriverWaveBuf* wavebuf = &audren->wavebufs[audren->next];
	if (samples != wavebuf->data_pcm16)
		return;
	armDCacheFlush(samples, wavebuf->size);
	mutexLock(&audren->mutex);
	audrvVoiceAddWaveBuf(&audren->driver, SWITCH_AUDREN_VOICE, wavebuf);
	if (!audren->started && !audren->paused)
		audrvVoiceStart(&audren->driver, SWITCH_AUDREN_VOICE);
	audren->started = true;
	audrvUpdate(&audren->driver);
	mutexUnlock(&audren->mutex);
	audren->next = (audren->next + 1) % audren->buffer_count;
}

void switchAudrenSetPaused(SwitchAudren* audren, bool paused)
{
	mutexLock(&audren->mutex);
	audren->paused = paused;
	if (audren->started)
	{
		audrvVoiceSetPaused(&audren->driver, SWITCH_AUDREN_VOICE, paused);
		audrvUpdate(&audren->driver);
	}
	mutexUnlock(&audren->mutex);
}

void switchAudrenClose(SwitchAudren* audren)
{
	if (!audren)
		return;
	mutexLock(&audren->mutex);
	audrvVoiceStop(&audren->driver, SWITCH_AUDREN_VOICE);
	audrvUpdate(&audren->driver);
	audrenStopAudioRenderer();
	audrvClose(&audren->driver);
	audrenExit();
	mutexUnlock(&audren->mutex);
	// (the renderer is closed: nothing reads the buffers any more)
	free(audren->pool);
	free(audren);
}

#endif // __SWITCH__
