// switch_audren: audio output through the audio renderer (audren) for the NX ports, on the Switch and the Switch 2.
// SPDX-License-Identifier: MIT
//
// One stereo s16 voice that the audio DSP resamples to its 48 kHz output, fed with buffers that the port fills on its
// own audio thread: switchAudrenAcquire waits for a free buffer, the port writes frames_per_buffer frames into it and
// queues it with switchAudrenSubmit. The audio renderer is one per process: nothing else (SDL audio included) may use
// it while the voice is open.
#pragma once

#if defined(__SWITCH__)

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SwitchAudren SwitchAudren;

// Starts the audio renderer with one voice at sample_rate and buffer_count buffers of frames_per_buffer stereo
// frames. NULL on failure, with the failing call's result in *result when result is not NULL.
SwitchAudren* switchAudrenOpen(uint32_t sample_rate, uint32_t frames_per_buffer, uint32_t buffer_count,
							   uint32_t* result);
// The next buffer to fill (frames_per_buffer interleaved stereo frames), waiting for the DSP to finish one when all
// are queued; NULL after timeout_ns without one
int16_t* switchAudrenAcquire(SwitchAudren* audren, uint64_t timeout_ns);
// Queues the buffer the last switchAudrenAcquire returned
void switchAudrenSubmit(SwitchAudren* audren, int16_t* samples);
// Pauses or resumes the voice (the queued buffers stay queued)
void switchAudrenSetPaused(SwitchAudren* audren, bool paused);
// Stops the voice and the audio renderer
void switchAudrenClose(SwitchAudren* audren);

#ifdef __cplusplus
}
#endif

#endif // __SWITCH__
