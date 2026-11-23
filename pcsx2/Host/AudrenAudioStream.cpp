// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Audio renderer (audren) backend for the Switch 2: one voice that the audio DSP resamples and mixes, fed by a thread
// of its own (switch/nx2/switch_audren).

#include "Host/AudioStream.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/Threading.h"

#include "nx2_gameshare.h"
#include "switch_audren.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>

namespace
{
	class AudrenAudioStream final : public AudioStream
	{
	public:
		AudrenAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~AudrenAudioStream() override;

		bool Initialize(bool stretch_enabled, Error* error);

	private:
		static constexpr u32 NUM_CHANNELS = 2;
		static constexpr u32 BUFFER_FRAMES = 512;
		static constexpr u32 BUFFER_SAMPLES = BUFFER_FRAMES * NUM_CHANNELS;
		static constexpr u32 NUM_BUFFERS = 4;

		void ThreadEntry();
		void FillBuffer(s16* out);
		void Destroy();

		SwitchAudren* m_audren = nullptr;
		std::thread m_thread;
		std::atomic_bool m_quit{false};
		std::array<SampleType, BUFFER_SAMPLES> m_float_scratch = {};
	};
} // namespace

AudrenAudioStream::AudrenAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

AudrenAudioStream::~AudrenAudioStream()
{
	Destroy();
}

void AudrenAudioStream::FillBuffer(s16* out)
{
	ReadFrames(m_float_scratch.data(), BUFFER_FRAMES);
	for (u32 i = 0; i < BUFFER_SAMPLES; i++)
	{
		const float f = std::clamp(m_float_scratch[i], -1.0f, 1.0f);
		out[i] = static_cast<s16>(std::lrintf(f * 32767.0f));
	}
	// GameShare's guest with a screen of its own hears the game too
	nx2GameShareWriteGuestAudioRate(out, BUFFER_FRAMES, m_sample_rate);
}

void AudrenAudioStream::ThreadEntry()
{
	Threading::SetNameOfCurrentThread("Audren");
	while (!m_quit.load(std::memory_order_relaxed))
	{
		s16* const buffer = switchAudrenAcquire(m_audren, 100'000'000ULL);
		if (!buffer)
			continue;
		FillBuffer(buffer);
		switchAudrenSubmit(m_audren, buffer);
	}
}

bool AudrenAudioStream::Initialize(bool stretch_enabled, Error* error)
{
	BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);

	u32 result = 0;
	m_audren = switchAudrenOpen(m_sample_rate, BUFFER_FRAMES, NUM_BUFFERS, &result);
	if (!m_audren)
	{
		Error::SetStringFmt(error, "The audio renderer could not start: 0x{:08x}", result);
		return false;
	}
	Console.WriteLn("audren started: %u Hz, %u x %u-frame buffers.", m_sample_rate, NUM_BUFFERS, BUFFER_FRAMES);

	m_quit.store(false, std::memory_order_relaxed);
	m_thread = std::thread(&AudrenAudioStream::ThreadEntry, this);
	return true;
}

void AudrenAudioStream::Destroy()
{
	if (m_thread.joinable())
	{
		m_quit.store(true, std::memory_order_relaxed);
		m_thread.join();
	}
	switchAudrenClose(m_audren);
	m_audren = nullptr;
}

std::unique_ptr<AudioStream> AudioStream::CreateAudrenAudioStream(u32 sample_rate,
	const AudioStreamParameters& parameters, bool stretch_enabled, Error* error)
{
	AudioStreamParameters params = parameters;
	params.expansion_mode = AudioExpansionMode::Disabled;

	std::unique_ptr<AudrenAudioStream> stream = std::make_unique<AudrenAudioStream>(sample_rate, params);
	if (!stream->Initialize(stretch_enabled, error))
		stream.reset();

	return stream;
}
