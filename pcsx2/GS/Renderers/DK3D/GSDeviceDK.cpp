// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DK3D/GSDeviceDK.h"

#include "GS/Renderers/Common/GSVertex.h"
#include "GS/GSPerfMon.h"
#include "PerformanceMetrics.h"

#include "imgui.h"

#ifdef __SWITCH__
#include "common/Console.h"
#include "common/Horizon/Horizon.h" // nwindowGetDefault()
#include "common/Timer.h"

#ifdef __SWITCH2__
#include "nx2_platform.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
	constexpr u32 CMDBUF_SIZE = 16 * 1024 * 1024;
	constexpr u32 CODE_MEMSIZE = 256 * 1024;
	constexpr u32 VERTEX_BUFFER_SIZE = 16 * 1024 * 1024;
	constexpr u32 INDEX_BUFFER_SIZE = 8 * 1024 * 1024;
	constexpr u32 UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024;
	// Holds a whole frame's texture uploads
	constexpr u32 STAGING_BUFFER_SIZE = 16 * 1024 * 1024;
	// Source base alignment for buffer to image copies
	constexpr u32 STAGING_ALIGNMENT = 256;

	constexpr u32 EXPAND_MAX_QUADS = 65536;
	constexpr u32 EXPAND_INDEX_BUFFER_SIZE = EXPAND_MAX_QUADS * 6 * sizeof(u32);

	// Each frame owns a start and end counter report
	constexpr u32 TIMESTAMP_REPORT_SIZE = 16;
	constexpr u32 TIMESTAMP_FRAME_STRIDE = 2 * TIMESTAMP_REPORT_SIZE;
	constexpr u32 TIMESTAMP_VALUE_OFFSET = 8;

	struct ConvertVertex
	{
		float pos[4];
		float uv[2];
	};

	struct DKTfxSelector
	{
		u32 fst, tme, tfx, tcc, atst, afail, fog, aem;
		u32 aem_fmt, pal_fmt, ltf, wms, wmt, dst_fmt, fba, iip;
		u32 region_rect, adjs, adjt, tcoffsethack;
		u32 blend_a, blend_b, blend_c, blend_d, blend_mix, blend_hw, pabe, fixed_one_a;
		u32 a_masked, colclip, colclip_hw, rta_correction, dither, dither_adjust, round_inv, tex_is_fb;
		u32 channel, shuffle, shuffle_same, read16src, process_ba, process_rg, shuffle_across, write_rg;
		u32 fbmask, scanmsk, date;
		u32 depth_fmt, urban_chaos, tales;
		u32 automatic_lod, manual_lod;
		u32 quantize_color, substitute_alpha, replacement_alpha_snap, native_texel_grid;
		u32 aa1, abe;
	};
	static_assert(sizeof(DKTfxSelector) == 232);
	static_assert(sizeof(GSHWDrawConfig::VSConstantBuffer) == 48);
	static_assert(sizeof(GSHWDrawConfig::PSConstantBuffer) == 288);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, ChannelShuffleOffset) == 144);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, TCOffsetHack) == 152);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, STScale) == 160);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, DitherMatrix) == 176);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, ScaleFactor) == 240);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, LineCovScale) == 256);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, SubstituteAlphaKeep) == 260);
	static_assert(offsetof(GSHWDrawConfig::PSConstantBuffer, NativeTexelGrid) == 272);

	// Translate a PSSelector into the tfx ubershader's cbSel uniform
	DKTfxSelector MakeTfxSelector(const GSHWDrawConfig::PSSelector& ps, bool has_tex)
	{
		DKTfxSelector sel = {};
		sel.fst = ps.fst;
		// tex_is_fb samples the RT without config.tex
		// Depth sources still need ST from the VS even when config.tex is a depth copy.
		sel.tme = (has_tex || ps.tex_is_fb || ps.depth_fmt != 0) ? 1u : 0u;
		sel.tfx = ps.tfx;
		sel.tcc = ps.tcc;
		sel.atst = static_cast<u32>(ps.atst);
		sel.afail = static_cast<u32>(ps.afail);
		sel.fog = ps.fog;
		sel.aem = ps.aem;
		sel.aem_fmt = ps.aem_fmt;
		sel.pal_fmt = ps.pal_fmt;
		sel.ltf = ps.ltf;
		sel.wms = ps.wms;
		sel.wmt = ps.wmt;
		sel.dst_fmt = ps.dst_fmt;
		sel.fba = ps.fba;
		sel.iip = ps.iip;
		sel.region_rect = ps.region_rect;
		sel.adjs = ps.adjs;
		sel.adjt = ps.adjt;
		sel.tcoffsethack = ps.tcoffsethack;
		sel.blend_a = ps.blend_a;
		sel.blend_b = ps.blend_b;
		sel.blend_c = ps.blend_c;
		sel.blend_d = ps.blend_d;
		sel.blend_mix = ps.blend_mix;
		sel.blend_hw = ps.blend_hw;
		sel.pabe = ps.pabe;
		sel.fixed_one_a = ps.fixed_one_a;
		sel.a_masked = ps.a_masked;
		sel.colclip = ps.colclip;
		sel.colclip_hw = ps.colclip_hw;
		sel.rta_correction = ps.rta_correction;
		sel.dither = ps.dither;
		sel.dither_adjust = ps.dither_adjust;
		sel.round_inv = ps.round_inv;
		sel.tex_is_fb = ps.tex_is_fb;
		sel.channel = ps.channel;
		sel.shuffle = ps.shuffle;
		sel.shuffle_same = ps.shuffle_same;
		sel.read16src = ps.real16src;
		sel.process_ba = ps.process_ba;
		sel.process_rg = ps.process_rg;
		sel.shuffle_across = ps.shuffle_across;
		sel.write_rg = ps.write_rg;
		sel.fbmask = ps.fbmask;
		sel.scanmsk = ps.scanmsk;
		sel.date = ps.date;
		sel.depth_fmt = ps.depth_fmt;
		sel.urban_chaos = ps.urban_chaos_hle;
		sel.tales = ps.tales_of_abyss_hle;
		sel.automatic_lod = ps.automatic_lod;
		sel.manual_lod = ps.manual_lod;
		sel.quantize_color = ps.quantize_color;
		sel.substitute_alpha = ps.substitute_alpha;
		sel.replacement_alpha_snap = ps.replacement_alpha_snap;
		sel.native_texel_grid = ps.native_texel_grid;
		sel.aa1 = (ps.aa1 == GSHWDrawConfig::PS_AA1::LINE) ? 1u : 0u;
		sel.abe = ps.abe;
		return sel;
	}

	constexpr DkVtxAttribState kTfxAttribs[7] = {
		{0, 0, 0, DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
		{0, 0, 8, DkVtxAttribSize_4x8, DkVtxAttribType_Uint, 0},
		{0, 0, 12, DkVtxAttribSize_1x32, DkVtxAttribType_Float, 0},
		{0, 0, 16, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
		{0, 0, 20, DkVtxAttribSize_1x32, DkVtxAttribType_Uint, 0},
		{0, 0, 24, DkVtxAttribSize_2x16, DkVtxAttribType_Uint, 0},
		{0, 0, 28, DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
	};
	constexpr DkVtxBufferState kTfxBufferState = {sizeof(GSVertex), 0};

	void BindTfxVertexInput(DkCmdBuf cmdbuf, GSHWDrawConfig::VSExpand expand)
	{
		const bool fetch = (expand == GSHWDrawConfig::VSExpand::None);
		dkCmdBufBindVtxAttribState(cmdbuf, kTfxAttribs, fetch ? 7 : 0);
		dkCmdBufBindVtxBufferState(cmdbuf, &kTfxBufferState, fetch ? 1 : 0);
	}

	constexpr DkPrimitive GetTfxPrimitive(GSHWDrawConfig::Topology topology)
	{
		switch (topology)
		{
			case GSHWDrawConfig::Topology::Point: return DkPrimitive_Points;
			case GSHWDrawConfig::Topology::Line: return DkPrimitive_Lines;
			default: return DkPrimitive_Triangles;
		}
	}

	constexpr DkBlendFactor kBlendFactors[16] = {
		DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor, DkBlendFactor_DstColor, DkBlendFactor_InvDstColor,
		DkBlendFactor_Src1Color, DkBlendFactor_InvSrc1Color, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
		DkBlendFactor_DstAlpha, DkBlendFactor_InvDstAlpha, DkBlendFactor_Src1Alpha, DkBlendFactor_InvSrc1Alpha,
		DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor, DkBlendFactor_One, DkBlendFactor_Zero,
	};
	constexpr DkBlendOp kBlendOps[3] = {DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_RevSub};

	constexpr bool IsIntegerOutput(ShaderConvert shader)
	{
		switch (shader)
		{
			case ShaderConvert::RGB5A1_TO_16_BITS:
			case ShaderConvert::DEPTH32_TO_16_BITS:
			case ShaderConvert::DEPTH32_TO_32_BITS:
				return true;
			default:
				return false;
		}
	}

	// These values are embedded in convert_fsh.glsl and convert_int_fsh.glsl.
	static_assert(static_cast<u32>(ShaderConvert::DEPTH_COPY) == 1);
	static_assert(static_cast<u32>(ShaderConvert::RGB5A1_TO_16_BITS) == 2);
	static_assert(static_cast<u32>(ShaderConvert::COLCLIP_INIT) == 7);
	static_assert(static_cast<u32>(ShaderConvert::COLCLIP_RESOLVE) == 8);
	static_assert(static_cast<u32>(ShaderConvert::RTA_CORRECTION) == 9);
	static_assert(static_cast<u32>(ShaderConvert::RTA_DECORRECTION) == 10);
	static_assert(static_cast<u32>(ShaderConvert::TRANSPARENCY_FILTER) == 11);
	static_assert(static_cast<u32>(ShaderConvert::DEPTH32_TO_16_BITS) == 12);
	static_assert(static_cast<u32>(ShaderConvert::DEPTH32_TO_32_BITS) == 13);
	static_assert(static_cast<u32>(ShaderConvert::DEPTH32_TO_RGBA8) == 14);
	static_assert(static_cast<u32>(ShaderConvert::DEPTH32_TO_RGB8) == 15);
	static_assert(static_cast<u32>(ShaderConvert::DEPTH16_TO_RGB5A1) == 16);
	static_assert(static_cast<u32>(ShaderConvert::RGBA8_TO_DEPTH32) == 17);
	static_assert(static_cast<u32>(ShaderConvert::RGBA8_TO_DEPTH24) == 18);
	static_assert(static_cast<u32>(ShaderConvert::RGBA8_TO_DEPTH16) == 19);
	static_assert(static_cast<u32>(ShaderConvert::RGB5A1_TO_DEPTH16) == 20);
	static_assert(static_cast<u32>(ShaderConvert::DEPTH32_TO_DEPTH24) == 21);

	constexpr u32 AlignUp(u32 value, u32 align)
	{
		return (value + align - 1) & ~(align - 1);
	}
} // namespace
#endif

GSDeviceDK::GSDeviceDK() = default;

GSDeviceDK::~GSDeviceDK()
{
#ifdef __SWITCH__
	DestroyDeviceObjects();
#endif
}

RenderAPI GSDeviceDK::GetRenderAPI() const
{
	return RenderAPI::DK3D;
}

bool GSDeviceDK::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	m_max_texture_size = 16384;
	// The swapchain's pacing reads them before GSDevice::Create sets them.
	m_vsync_mode = vsync_mode;
	m_allow_present_throttle = allow_present_throttle;

	m_window_info.type = WindowInfo::Type::Android;
#ifdef __SWITCH2__
	// The window nx2DisplayInit set up: 1920x1080 handheld, the TV's resolution docked.
	m_name = "drako3d";
	u32 display_width = 1920, display_height = 1080;
	nx2DisplaySize(&display_width, &display_height);
	m_window_info.surface_width = display_width;
	m_window_info.surface_height = display_height;
	m_window_info.surface_refresh_rate = static_cast<float>(nx2DisplayRefreshRate());
#else
	m_name = "deko3d";
	m_window_info.surface_width = 1280;
	m_window_info.surface_height = 720;
	m_window_info.surface_refresh_rate = 60.0f;
#endif
	m_window_info.surface_scale = 1.0f;

#ifdef __SWITCH__
	m_present_width = static_cast<int>(m_window_info.surface_width);
	m_present_height = static_cast<int>(m_window_info.surface_height);
	if (!CreateDeviceObjects())
	{
		Console.Error("DK3D: failed to create deko3d device objects.");
		DestroyDeviceObjects();
		return false;
	}
#endif

	return GSDevice::Create(vsync_mode, allow_present_throttle);
}

#ifdef __SWITCH__
bool GSDeviceDK::CreateDeviceObjects()
{
	DkDeviceMaker device_maker;
	dkDeviceMakerDefaults(&device_maker);
	// Log errors for when this inevitably breaks some games and issues are opened regarding them.
	device_maker.cbDebug = [](void* userData, const char* context, DkResult result, const char* message) {
		Console.Error("DK3D deko3d error: context='%s' result=%d message='%s'", context ? context : "?",
			static_cast<int>(result), message ? message : "?");
	};
#ifdef __SWITCH2__
	// The launcher, another program of this process, presented with the GPU before: drako3d opens it on a new GSV
	// root once Mesa's device is retired and the VI2 layer released.
	if (!nx2GpuReleaseForDrako3d())
		Console.Warning("DK3D: the VI2 layer could not be released before drako3d opened the GPU");
#endif
	Horizon::Breadcrumb("DK3D: dkDeviceCreate");
	m_device = dkDeviceCreate(&device_maker);
	if (!m_device)
		return false;
	Horizon::Breadcrumb("DK3D: device created");

	if (!CreateSwapchain())
		return false;

	DkMemBlockMaker memblock_maker;

	// Successive frames overlap instead of serialising through dkQueueWaitIdle.
	DkCmdBufMaker cmdbuf_maker;
	dkCmdBufMakerDefaults(&cmdbuf_maker, m_device);
	cmdbuf_maker.userData = this;
	cmdbuf_maker.cbAddMem = &GSDeviceDK::AddCmdMemoryThunk;

	for (unsigned i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
	{
		FrameContext& ctx = m_frames[i];

		dkMemBlockMakerDefaults(&memblock_maker, m_device, CMDBUF_SIZE);
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		ctx.cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);
		if (!ctx.cmdbuf_memblock)
			return false;

		ctx.cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
		if (!ctx.cmdbuf)
			return false;
		dkCmdBufAddMemory(ctx.cmdbuf, ctx.cmdbuf_memblock, 0, CMDBUF_SIZE);

		dkMemBlockMakerDefaults(&memblock_maker, m_device, VERTEX_BUFFER_SIZE);
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		ctx.vertex_memblock = dkMemBlockCreate(&memblock_maker);
		if (!ctx.vertex_memblock)
			return false;

		dkMemBlockMakerDefaults(&memblock_maker, m_device, INDEX_BUFFER_SIZE);
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		ctx.index_memblock = dkMemBlockCreate(&memblock_maker);
		if (!ctx.index_memblock)
			return false;

		dkMemBlockMakerDefaults(&memblock_maker, m_device, UNIFORM_BUFFER_SIZE);
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		ctx.uniform_memblock = dkMemBlockCreate(&memblock_maker);
		if (!ctx.uniform_memblock)
			return false;

		dkMemBlockMakerDefaults(&memblock_maker, m_device, STAGING_BUFFER_SIZE);
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		ctx.staging_memblock = dkMemBlockCreate(&memblock_maker);
		if (!ctx.staging_memblock)
			return false;

		dkMemBlockMakerDefaults(&memblock_maker, m_device,
			AlignUp(NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor), DK_MEMBLOCK_ALIGNMENT));
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		ctx.descriptor_memblock = dkMemBlockCreate(&memblock_maker);
		if (!ctx.descriptor_memblock)
			return false;
	}

	// BeginFrameIfNeeded rotates frame aliases through NUM_FRAMES_IN_FLIGHT contexts
	m_frame_index = 0;
	m_cmdbuf = m_frames[0].cmdbuf;
	m_cmdbuf_memblock = m_frames[0].cmdbuf_memblock;
	m_vertex_memblock = m_frames[0].vertex_memblock;
	m_index_memblock = m_frames[0].index_memblock;
	m_uniform_memblock = m_frames[0].uniform_memblock;
	m_staging_memblock = m_frames[0].staging_memblock;

	// Graphics and computre queue
	DkQueueMaker queue_maker;
	dkQueueMakerDefaults(&queue_maker, m_device);
	queue_maker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;
#ifdef __SWITCH2__
	// LSFG 2x's compute passes need more scratch than the default.
	SwitchLSFG_Configure(true);
	if (SwitchLSFG_IsPrepared())
		queue_maker.perWarpScratchMemorySize = LSFG_SCRATCH_PER_WARP;
#endif
	Horizon::Breadcrumb("DK3D: dkQueueCreate");
	m_queue = dkQueueCreate(&queue_maker);
	if (!m_queue)
		return false;
	Horizon::Breadcrumb("DK3D: queue created");

	const u32 descriptor_size = AlignUp(NUM_SAMPLERS * sizeof(DkSamplerDescriptor), DK_MEMBLOCK_ALIGNMENT);
	dkMemBlockMakerDefaults(&memblock_maker, m_device, descriptor_size);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_sampler_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_sampler_memblock)
		return false;
	m_sampler_descriptor_set = dkMemBlockGetGpuAddr(m_sampler_memblock);

	// GPU-timing report buffer
	dkMemBlockMakerDefaults(&memblock_maker, m_device, DK_MEMBLOCK_ALIGNMENT);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	m_timestamp_memblock = dkMemBlockCreate(&memblock_maker);
	if (m_timestamp_memblock)
		std::memset(dkMemBlockGetCpuAddr(m_timestamp_memblock), 0, DK_MEMBLOCK_ALIGNMENT);

	// Allow GSRendererHW to use software blending
	m_features.texture_barrier = true;
	m_features.present_from_back_thread = true;

	Horizon::Breadcrumb("DK3D: LoadShaders");
	LoadShaders();
	Horizon::Breadcrumb("DK3D: LoadShaders done");
	if (!m_convert_shaders_ok)
		Console.Warning("DK3D: No shaders available. Things will be broken.");
	else
		SetupSamplers();

	m_features.cas_sharpening = m_cas_shader_ok;

	if (m_tfx_expand_shaders_ok && !GSConfig.DisableVertexShaderExpand)
	{
		dkMemBlockMakerDefaults(&memblock_maker, m_device, AlignUp(EXPAND_INDEX_BUFFER_SIZE, DK_MEMBLOCK_ALIGNMENT));
		memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
		m_expand_index_memblock = dkMemBlockCreate(&memblock_maker);
		if (m_expand_index_memblock)
		{
			u32* idx = static_cast<u32*>(dkMemBlockGetCpuAddr(m_expand_index_memblock));
			for (u32 i = 0; i < EXPAND_MAX_QUADS; i++)
			{
				const u32 base = i * 4;
				*(idx++) = base + 0;
				*(idx++) = base + 1;
				*(idx++) = base + 2;
				*(idx++) = base + 1;
				*(idx++) = base + 2;
				*(idx++) = base + 3;
			}
		}
	}
	m_features.vs_expand = (m_expand_index_memblock != nullptr);

	// DATE shaders
	m_features.stencil_buffer = m_date_shaders_ok;
	m_features.primitive_id = m_date_shaders_ok && m_tfx_shaders_ok;

	Console.WriteLn("DK3D: deko3d device up (%dx%d, %u framebuffers, convert=%d, tfx=%d, vs_expand=%d).",
		m_present_width, m_present_height, NUM_FRAMEBUFFERS, m_convert_shaders_ok ? 1 : 0, m_tfx_shaders_ok ? 1 : 0,
		m_features.vs_expand ? 1 : 0);
	return true;
}

bool GSDeviceDK::SetupSamplers()
{
	// tau/tav select REPEAT vs CLAMP per axis.
	DkSamplerDescriptor descriptors[NUM_SAMPLERS];
	for (unsigned i = 0; i < NUM_SAMPLERS; i++)
	{
		const bool biln = (i & 4) != 0;
		const bool tau = (i & 2) != 0;
		const bool tav = (i & 1) != 0;

		DkSampler sampler;
		dkSamplerDefaults(&sampler);
		sampler.minFilter = biln ? DkFilter_Linear : DkFilter_Nearest;
		sampler.magFilter = biln ? DkFilter_Linear : DkFilter_Nearest;
		sampler.mipFilter = DkMipFilter_Linear;
		sampler.wrapMode[0] = tau ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
		sampler.wrapMode[1] = tav ? DkWrapMode_Repeat : DkWrapMode_ClampToEdge;
		sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
		dkSamplerDescriptorInitialize(&descriptors[i], &sampler);
	}

	// Samplers never change
	std::memcpy(dkMemBlockGetCpuAddr(m_sampler_memblock), descriptors, sizeof(descriptors));
	return true;
}

void GSDeviceDK::IssueBarrier(DkBarrier mode, u32 invalidate_flags)
{
	dkCmdBufBarrier(m_cmdbuf, mode, invalidate_flags);
	if (invalidate_flags & DkInvalidateFlags_Image)
		m_gpu_write_gen++;
#ifdef __SWITCH2__
	if (mode == DkBarrier_Full)
		m_copy_engine_busy = m_3d_since_full_barrier = false;
#endif
}

void GSDeviceDK::WriteGPUTimestamp(u32 frame_index, u32 which)
{
	if (!m_timestamp_memblock)
		return;
	const DkGpuAddr addr = dkMemBlockGetGpuAddr(m_timestamp_memblock) +
						   frame_index * TIMESTAMP_FRAME_STRIDE + which * TIMESTAMP_REPORT_SIZE;
	dkCmdBufReportCounter(m_cmdbuf, DkCounter_Timestamp, addr);
}

void GSDeviceDK::ReadGPUTimestamps(u32 frame_index)
{
	if (!m_timestamp_memblock)
		return;
	const u8* base = static_cast<const u8*>(dkMemBlockGetCpuAddr(m_timestamp_memblock)) +
					 frame_index * TIMESTAMP_FRAME_STRIDE;

	u64 start, end;
	std::memcpy(&start, base + TIMESTAMP_VALUE_OFFSET, sizeof(start));
	std::memcpy(&end, base + TIMESTAMP_REPORT_SIZE + TIMESTAMP_VALUE_OFFSET, sizeof(end));
	if (end > start)
		m_accumulated_gpu_time += static_cast<float>(static_cast<double>(dkTimestampToNs(end - start)) / 1000000.0);
}

void GSDeviceDK::BeginFrameIfNeeded()
{
	if (m_frame_active)
		return;

	m_frame_index = (m_frame_index + 1) % NUM_FRAMES_IN_FLIGHT;
	FrameContext& ctx = m_frames[m_frame_index];
	if (ctx.fence_pending)
	{
		const Common::Timer::Value wait_start = Common::Timer::GetCurrentValue();
		dkFenceWait(&ctx.fence, -1);
		PerformanceMetrics::AccumulateGSGpuWait(
			static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - wait_start)));
		ctx.fence_pending = false;
		m_completed_serial = std::max(m_completed_serial, ctx.serial);
		for (DkMemBlock block : ctx.extra_cmdbuf_memblocks)
			dkMemBlockDestroy(block);
		ctx.extra_cmdbuf_memblocks.clear();
		for (const PooledMemBlock& block : ctx.retired_memblocks)
			PoolMemBlock(block);
		ctx.retired_memblocks.clear();
		// Frame is now complete
		if (ctx.timestamp_written)
		{
			ReadGPUTimestamps(m_frame_index);
			ctx.timestamp_written = false;
		}
	}

	m_cmdbuf = ctx.cmdbuf;
	m_cmdbuf_memblock = ctx.cmdbuf_memblock;
	m_vertex_memblock = ctx.vertex_memblock;
	m_index_memblock = ctx.index_memblock;
	m_uniform_memblock = ctx.uniform_memblock;
	m_staging_memblock = ctx.staging_memblock;
	m_descriptor_memblock = ctx.descriptor_memblock;
	m_image_descriptor_set = dkMemBlockGetGpuAddr(m_descriptor_memblock);

	dkCmdBufClear(m_cmdbuf);
	dkCmdBufAddMemory(m_cmdbuf, m_cmdbuf_memblock, 0, CMDBUF_SIZE);
	m_vertex_offset = 0;
	m_index_offset = 0;
	m_uniform_offset = 0;
	m_staging_offset = 0;
	m_next_image_slot = 0;
	// Refresh command buffer
	InvalidateHWStateCache();
	// The descriptor slots restart in a block the GPU read three frames ago, and binding a descriptor set does not
	// invalidate the texture header and sampler caches.
	IssueBarrier(DkBarrier_None, DkInvalidateFlags_Descriptors);

	m_frame_active = true;

	// First command of the fresh list
	if (m_gpu_timing_enabled)
	{
		WriteGPUTimestamp(m_frame_index, 0);
		ctx.timestamp_written = true;
	}
}

void GSDeviceDK::AddCmdMemoryThunk(void* userData, DkCmdBuf cmdbuf, size_t minReqSize)
{
	static_cast<GSDeviceDK*>(userData)->AddCmdMemory(cmdbuf, minReqSize);
}

void GSDeviceDK::AddCmdMemory(DkCmdBuf cmdbuf, size_t minReqSize)
{
	const u32 size = AlignUp(std::max<u32>(static_cast<u32>(minReqSize), CMDBUF_SIZE), DK_MEMBLOCK_ALIGNMENT);
	Console.Warning("DK3D: command buffer overflowed (need %zu bytes). Allocating %u more.", minReqSize, size);

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, size);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	DkMemBlock block = dkMemBlockCreate(&memblock_maker);
	if (!block)
	{
		Console.Error("DK3D: failed to allocate overflow command memory.");
		return;
	}

	m_frames[m_frame_index].extra_cmdbuf_memblocks.push_back(block);
	dkCmdBufAddMemory(cmdbuf, block, 0, size);
}

DkMemBlock GSDeviceDK::AllocateFrameMemory(u32 size)
{
	const u32 block_size = AlignUp(size, DK_MEMBLOCK_ALIGNMENT);
	const u32 block_flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	DkMemBlock block = AcquireMemBlock(block_size, block_flags);
	if (!block)
		pxFailRel("DK3D: failed to allocate frame stream memory.");
	m_frames[m_frame_index].retired_memblocks.push_back({block, block_size, block_flags});
	return block;
}

void* GSDeviceDK::ReserveStreamMemory(DkMemBlock& block, u32& offset, u32 size, u32 alignment,
	u32 capacity, DkGpuAddr* gpu_addr)
{
	offset = AlignUp(offset, alignment);
	if (size > dkMemBlockGetSize(block) - offset)
	{
		// Earlier commands keep referencing the old block until this frame's fence completes.
		block = AllocateFrameMemory(std::max(size, capacity));
		offset = 0;
	}
	void* const cpu_addr = static_cast<u8*>(dkMemBlockGetCpuAddr(block)) + offset;
	*gpu_addr = dkMemBlockGetGpuAddr(block) + offset;
	offset += size;
	return cpu_addr;
}

DkGpuAddr GSDeviceDK::StreamVertices(const void* data, u32 size)
{
	DkGpuAddr addr;
	void* dst = ReserveStreamMemory(m_vertex_memblock, m_vertex_offset, size, alignof(GSVertex),
		VERTEX_BUFFER_SIZE, &addr);
	std::memcpy(dst, data, size);
	return addr;
}

DkGpuAddr GSDeviceDK::StreamIndices(const void* data, u32 size)
{
	DkGpuAddr addr;
	void* dst = ReserveStreamMemory(m_index_memblock, m_index_offset, size, sizeof(u32), INDEX_BUFFER_SIZE, &addr);
	std::memcpy(dst, data, size);
	return addr;
}

DkGpuAddr GSDeviceDK::StreamUniform(const void* data, u32 size)
{
	DkGpuAddr addr;
	void* dst = ReserveStreamMemory(m_uniform_memblock, m_uniform_offset, AlignUp(size, DK_UNIFORM_BUF_ALIGNMENT),
		DK_UNIFORM_BUF_ALIGNMENT, UNIFORM_BUFFER_SIZE, &addr);
	std::memcpy(dst, data, size);
	return addr;
}

void GSDeviceDK::BindHWGeometry(const GSHWDrawConfig& config)
{
	const u32 vtx_size = config.nverts * sizeof(GSVertex);
	const DkGpuAddr vtx_addr = StreamVertices(config.verts, vtx_size);
	if (config.vs.expand == GSHWDrawConfig::VSExpand::None)
		dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vtx_addr, vtx_size);
	else
		dkCmdBufBindStorageBuffer(m_cmdbuf, DkStage_Vertex, 0, vtx_addr, vtx_size);

	if (config.vs.UseFixedExpandIndexBuffer())
	{
		pxAssert(config.nindices <= EXPAND_MAX_QUADS * 6);
		dkCmdBufBindIdxBuffer(m_cmdbuf, DkIdxFormat_Uint32, dkMemBlockGetGpuAddr(m_expand_index_memblock));
	}
	else
	{
		dkCmdBufBindIdxBuffer(m_cmdbuf, DkIdxFormat_Uint16,
			StreamIndices(config.indices, config.nindices * sizeof(u16)));
	}
}

void GSDeviceDK::ReserveImageDescriptors(u32 count)
{
	pxAssert(count <= NUM_IMAGE_DESCRIPTORS);
	if (m_next_image_slot + count <= NUM_IMAGE_DESCRIPTORS)
		return;

	m_descriptor_memblock = AllocateFrameMemory(NUM_IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor));
	m_image_descriptor_set = dkMemBlockGetGpuAddr(m_descriptor_memblock);
	m_next_image_slot = 0;
	m_hw_tex_valid = false;
	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	IssueBarrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
}

u32 GSDeviceDK::PushImageDescriptor(const DkImageDescriptor& descriptor)
{
	pxAssert(m_next_image_slot < NUM_IMAGE_DESCRIPTORS);
	const u32 slot = m_next_image_slot++;
	static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(m_descriptor_memblock))[slot] = descriptor;
	return slot;
}

u32 GSDeviceDK::PushImage(const GSTextureDK* tex)
{
	return PushImageDescriptor(tex->GetDescriptor());
}

void GSDeviceDK::CommitClear(GSTextureDK* tex)
{
	if (!tex || tex->GetState() != GSTexture::State::Cleared)
		return;

	WaitForCopyEngine();
	DkImageView view;
	tex->GetImageView(&view);
	if (tex->IsDepth())
		dkCmdBufBindRenderTargets(m_cmdbuf, nullptr, 0, &view);
	else
		dkCmdBufBindRenderTarget(m_cmdbuf, &view, nullptr);
	m_hw_targets_valid = false;
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(tex->GetWidth()), static_cast<float>(tex->GetHeight()),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(tex->GetWidth()), static_cast<u32>(tex->GetHeight())};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	if (tex->IsDepth())
		dkCmdBufClearDepthStencil(m_cmdbuf, true, tex->GetClearDepth(), 0, 0);
	else if (tex->GetFormat() == GSTexture::Format::UInt16 || tex->GetFormat() == GSTexture::Format::UInt32)
		dkCmdBufClearColorUint(m_cmdbuf, 0, DkColorMask_RGBA, tex->GetClearColor(), 0, 0, 0);
	else
	{
		float cc[4];
		GSVector4::store<false>(cc, tex->GetClearForFormat());
		dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, cc[0], cc[1], cc[2], cc[3]);
	}
	tex->SetState(GSTexture::State::Dirty);
	tex->SetWriteGen(m_gpu_write_gen);
}

void GSDeviceDK::DoStretchRectImpl(GSTextureDK* sTex, const GSVector4& sRect, GSTextureDK* dTex,
	const GSVector4& dRect, const DkShader* fragment_shader, bool linear, const void* cb, u32 cb_size,
	bool depth_output, u32 color_write_mask, bool alpha_blend)
{
	if (!sTex || !m_convert_shaders_ok)
		return;

	// Depth output needs a destination depth image
	if (depth_output && (!dTex || !dTex->IsDepth()))
		return;

	BeginFrameIfNeeded();
	WaitForCopyEngine();
	// This pass binds convert shaders/blend/vertex layout
	InvalidateHWStateCache();

	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	// Flush only when sTex was written this generation
	CommitClear(sTex);
	CommitClear(dTex);
	if (sTex->GetWriteGen() == m_gpu_write_gen)
	{
		IssueBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
		g_perfmon.Put(GSPerfMon::Barriers, 1);
	}

	const bool is_present = (dTex == nullptr);
	const GSVector2i ds = is_present ? GSVector2i(m_present_width, m_present_height) : dTex->GetSize();

	DkImageView target_view;
	if (is_present)
		target_view = m_swapchain_view;
	else
		dTex->GetImageView(&target_view);
	if (depth_output)
		dkCmdBufBindRenderTargets(m_cmdbuf, nullptr, 0, &target_view);
	else
		dkCmdBufBindRenderTarget(m_cmdbuf, &target_view, nullptr);

	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(ds.x), static_cast<float>(ds.y), 0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(ds.x), static_cast<u32>(ds.y)};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);

	DkRasterizerState rasterizer_state;
	DkColorState color_state;
	DkColorWriteState color_write_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	dkColorStateDefaults(&color_state);
	dkColorWriteStateDefaults(&color_write_state);
	// The default culling mode discards quads too frequently
	rasterizer_state.cullMode = DkFace_None;
	dkColorWriteStateSetMask(&color_write_state, 0, color_write_mask);
	if (alpha_blend)
		dkColorStateSetBlendEnable(&color_state, 0, true);
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);
	if (alpha_blend)
	{
		DkBlendState blend_state;
		dkBlendStateDefaults(&blend_state);
		dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);
	}

	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	depth_state.depthTestEnable = depth_output;
	depth_state.depthWriteEnable = depth_output;
	depth_state.depthCompareOp = DkCompareOp_Always;
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);

	const DkShader* shaders[] = {&m_convert_vsh, fragment_shader};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	ReserveImageDescriptors(1);
	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	const u32 image_slot = PushImage(sTex);
	const DkResHandle texture_handle = dkMakeTextureHandle(image_slot, linear ? SAMPLER_LINEAR : SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &texture_handle, 1);

	// Optional post-process constants.
	if (cb && cb_size)
	{
		const DkGpuAddr cb_addr = StreamUniform(cb, cb_size);
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 0, cb_addr, AlignUp(cb_size, DK_UNIFORM_BUF_ALIGNMENT));
	}

	const float left = dRect.x * 2.0f / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2.0f / ds.y;
	const float right = dRect.z * 2.0f / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2.0f / ds.y;
	const ConvertVertex vertices[4] = {
		{{left, top, 0.5f, 1.0f}, {sRect.x, sRect.y}},
		{{right, top, 0.5f, 1.0f}, {sRect.z, sRect.y}},
		{{left, bottom, 0.5f, 1.0f}, {sRect.x, sRect.w}},
		{{right, bottom, 0.5f, 1.0f}, {sRect.z, sRect.w}},
	};

	const DkGpuAddr vertex_addr = StreamVertices(vertices, sizeof(vertices));

	static const DkVtxAttribState attribs[2] = {
		{0, 0, offsetof(ConvertVertex, pos), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ConvertVertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
	};
	static const DkVtxBufferState buffer_state = {sizeof(ConvertVertex), 0};
	dkCmdBufBindVtxAttribState(m_cmdbuf, attribs, 2);
	dkCmdBufBindVtxBufferState(m_cmdbuf, &buffer_state, 1);
	dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vertex_addr, sizeof(vertices));
	WaitForCopyEngine();
	dkCmdBufDraw(m_cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);

	if (!is_present)
	{
		dTex->SetState(GSTexture::State::Dirty);
		dTex->SetWriteGen(m_gpu_write_gen);
	}
}

bool GSDeviceDK::LoadShaders()
{
	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, CODE_MEMSIZE);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
	m_code_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_code_memblock)
		return false;

	u32 code_offset = 0;
	const auto load_one = [&](DkShader& shader, const char* path) -> bool {
		std::FILE* f = std::fopen(path, "rb");
		if (!f)
		{
			Console.Error("DK3D: could not open shader %s", path);
			return false;
		}
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::rewind(f);
		if (size <= 0)
		{
			std::fclose(f);
			return false;
		}

		const u32 offset = code_offset;
		code_offset += (static_cast<u32>(size) + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
		if (code_offset > CODE_MEMSIZE - DK_SHADER_CODE_UNUSABLE_SIZE)
		{
			std::fclose(f);
			Console.Error("DK3D: shader code memblock too small for %s", path);
			return false;
		}

		void* dst = static_cast<u8*>(dkMemBlockGetCpuAddr(m_code_memblock)) + offset;
		const size_t read = std::fread(dst, 1, static_cast<size_t>(size), f);
		std::fclose(f);
		if (read != static_cast<size_t>(size))
			return false;

		DkShaderMaker shader_maker;
		dkShaderMakerDefaults(&shader_maker, m_code_memblock, offset);
		dkShaderInitialize(&shader, &shader_maker);
		return true;
	};

	m_convert_shaders_ok = load_one(m_convert_vsh, "romfs:/shaders/convert_vsh.dksh") &&
						   load_one(m_copy_fsh, "romfs:/shaders/texture_fsh.dksh") &&
						   load_one(m_convert_fsh, "romfs:/shaders/convert_fsh.dksh") &&
						   load_one(m_convert_int_fsh, "romfs:/shaders/convert_int_fsh.dksh");

	if (m_convert_shaders_ok)
		Console.WriteLn("DK3D: convert shaders loaded.");

	m_texconvert_shaders_ok = load_one(m_downsample_fsh, "romfs:/shaders/downsample_fsh.dksh") &&
							  load_one(m_clut_fsh, "romfs:/shaders/clut_fsh.dksh") &&
							  load_one(m_convert_8i_fsh, "romfs:/shaders/convert_8i_fsh.dksh");

	if (m_texconvert_shaders_ok)
		Console.WriteLn("DK3D: texture cache convert shaders loaded.");
	else
		Console.Warning("DK3D: texture cache convert shaders missing. CLUT/downsample disabled.");

	m_date_shaders_ok = load_one(m_date_fsh, "romfs:/shaders/date_fsh.dksh") &&
						load_one(m_primid_init_fsh, "romfs:/shaders/primid_init_fsh.dksh");

	if (m_date_shaders_ok)
		Console.WriteLn("DK3D: DATE setup shaders loaded.");
	else
		Console.Warning("DK3D: DATE setup shaders missing. Using slower COPY shader instead.");

	m_tfx_shaders_ok = load_one(m_tfx_vsh[0], "romfs:/shaders/tfx_vsh.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantUber], "romfs:/shaders/tfx_fsh.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantOpaque], "romfs:/shaders/tfx_fsh_opaque.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantPalette], "romfs:/shaders/tfx_fsh_palette.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantFast], "romfs:/shaders/tfx_fsh_fast.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantFeedback], "romfs:/shaders/tfx_fsh_feedback.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantFeedbackPalette], "romfs:/shaders/tfx_fsh_feedback_palette.dksh") &&
					   load_one(m_tfx_fsh[TfxVariantFeedbackFast], "romfs:/shaders/tfx_fsh_feedback_fast.dksh");

	if (m_tfx_shaders_ok)
		Console.WriteLn("DK3D: tfx shaders loaded.");
	else
		Console.Warning("DK3D: tfx shaders missing. Things will be broken.");

	using VSExpand = GSHWDrawConfig::VSExpand;
	m_tfx_expand_shaders_ok = m_tfx_shaders_ok &&
		load_one(m_tfx_vsh[static_cast<u32>(VSExpand::Point)], "romfs:/shaders/tfx_vsh_expand_point.dksh") &&
		load_one(m_tfx_vsh[static_cast<u32>(VSExpand::Line)], "romfs:/shaders/tfx_vsh_expand_line.dksh") &&
		load_one(m_tfx_vsh[static_cast<u32>(VSExpand::Sprite)], "romfs:/shaders/tfx_vsh_expand_sprite.dksh") &&
		load_one(m_tfx_vsh[static_cast<u32>(VSExpand::LineAA1)], "romfs:/shaders/tfx_vsh_expand_line_aa1.dksh");

	if (!m_tfx_expand_shaders_ok)
		Console.Warning("DK3D: tfx expand shaders missing. Points, lines and sprites expand on the CPU.");

	m_postprocess_shaders_ok = load_one(m_interlace_fsh, "romfs:/shaders/interlace_fsh.dksh") &&
							   load_one(m_shadeboost_fsh, "romfs:/shaders/shadeboost_fsh.dksh") &&
							   load_one(m_fxaa_fsh, "romfs:/shaders/fxaa_fsh.dksh") &&
							   load_one(m_merge_fsh, "romfs:/shaders/merge_fsh.dksh") &&
							   load_one(m_yuv_fsh, "romfs:/shaders/yuv_fsh.dksh");

	if (m_postprocess_shaders_ok)
		Console.WriteLn("DK3D: post-process shaders loaded.");
	else
		Console.Warning("DK3D: post-process shaders missing. Enhancements won't work properly.");

	m_imgui_shaders_ok = load_one(m_imgui_vsh, "romfs:/shaders/imgui_vsh.dksh") &&
						 load_one(m_imgui_fsh, "romfs:/shaders/imgui_fsh.dksh");

	if (m_imgui_shaders_ok)
		Console.WriteLn("DK3D: imgui shaders loaded.");
	else
		Console.Warning("DK3D: imgui shaders missing. No UI will be present.");

	m_cas_shader_ok = load_one(m_cas_csh, "romfs:/shaders/cas_csh.dksh");

	if (m_cas_shader_ok)
		Console.WriteLn("DK3D: CAS shader loaded.");
	else
		Console.Warning("DK3D: CAS shader missing. Contrast adaptive sharpening is unavailable.");

	return m_convert_shaders_ok;
}

bool GSDeviceDK::CreateSwapchain()
{
	DkImageLayoutMaker layout_maker;
	dkImageLayoutMakerDefaults(&layout_maker, m_device);
#ifdef __SWITCH2__
	// drako3d swapchain images cannot use hardware compression; LSFG 2x blits into them.
	layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
#else
	layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
#endif
	layout_maker.format = DkImageFormat_RGBA8_Unorm;
	layout_maker.dimensions[0] = m_present_width;
	layout_maker.dimensions[1] = m_present_height;

	DkImageLayout framebuffer_layout;
	dkImageLayoutInitialize(&framebuffer_layout, &layout_maker);

	u32 fb_size = dkImageLayoutGetSize(&framebuffer_layout);
	const u32 fb_align = dkImageLayoutGetAlignment(&framebuffer_layout);
	fb_size = (fb_size + fb_align - 1) & ~(fb_align - 1);
	m_framebuffer_size = fb_size;

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, NUM_FRAMEBUFFERS * fb_size);
	memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	m_fb_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_fb_memblock)
		return false;

	// Initialise the framebuffer images and build a swapchain from them.
	const DkImage* swapchain_images[NUM_FRAMEBUFFERS];
	for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++)
	{
		dkImageInitialize(&m_framebuffers[i], &framebuffer_layout, m_fb_memblock, i * fb_size);
		swapchain_images[i] = &m_framebuffers[i];
	}

	DkSwapchainMaker swapchain_maker;
	dkSwapchainMakerDefaults(&swapchain_maker, m_device, nwindowGetDefault(), swapchain_images, NUM_FRAMEBUFFERS);
	m_swapchain = dkSwapchainCreate(&swapchain_maker);
	if (!m_swapchain)
	{
		dkMemBlockDestroy(m_fb_memblock);
		m_fb_memblock = nullptr;
		return false;
	}
#ifdef __SWITCH2__
	// Creating a swapchain resets the window's present interval: reapply nx2's pacing.
	nx2DisplayApply(m_vsync_mode == GSVSyncMode::Disabled ? 0 : 1);
#endif
	return true;
}

void GSDeviceDK::DestroySwapchain()
{
	if (m_swapchain)
	{
		dkSwapchainDestroy(m_swapchain);
		m_swapchain = nullptr;
	}
	if (m_fb_memblock)
	{
		dkMemBlockDestroy(m_fb_memblock);
		m_fb_memblock = nullptr;
	}
}

void GSDeviceDK::DestroyDeviceObjects()
{
#ifdef __SWITCH2__
	CloseGuestScreen();
	StopFrameGenerationPreparation();
	DestroyFrameGeneration();
	SwitchLSFG_SetStatus(false, false);
	// The launcher starts at the TV's own resolution.
	if (nx2DisplaySetFrameGeneration(false))
		nx2DisplayReapply();
#endif
	if (m_queue)
		dkQueueWaitIdle(m_queue);

	// Per-frame contexts
	for (unsigned i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
	{
		FrameContext& ctx = m_frames[i];
		if (ctx.cmdbuf)
		{
			dkCmdBufDestroy(ctx.cmdbuf);
			ctx.cmdbuf = nullptr;
		}
		for (DkMemBlock block : ctx.extra_cmdbuf_memblocks)
			dkMemBlockDestroy(block);
		ctx.extra_cmdbuf_memblocks.clear();
		for (const PooledMemBlock& block : ctx.retired_memblocks)
			dkMemBlockDestroy(block.block);
		ctx.retired_memblocks.clear();
		if (ctx.descriptor_memblock)
		{
			dkMemBlockDestroy(ctx.descriptor_memblock);
			ctx.descriptor_memblock = nullptr;
		}
		if (ctx.cmdbuf_memblock)
		{
			dkMemBlockDestroy(ctx.cmdbuf_memblock);
			ctx.cmdbuf_memblock = nullptr;
		}
		if (ctx.vertex_memblock)
		{
			dkMemBlockDestroy(ctx.vertex_memblock);
			ctx.vertex_memblock = nullptr;
		}
		if (ctx.index_memblock)
		{
			dkMemBlockDestroy(ctx.index_memblock);
			ctx.index_memblock = nullptr;
		}
		if (ctx.uniform_memblock)
		{
			dkMemBlockDestroy(ctx.uniform_memblock);
			ctx.uniform_memblock = nullptr;
		}
		if (ctx.staging_memblock)
		{
			dkMemBlockDestroy(ctx.staging_memblock);
			ctx.staging_memblock = nullptr;
		}
		ctx.fence_pending = false;
	}
	m_cmdbuf = nullptr;
	m_cmdbuf_memblock = nullptr;
	m_vertex_memblock = nullptr;
	m_index_memblock = nullptr;
	m_uniform_memblock = nullptr;
	m_staging_memblock = nullptr;

	m_descriptor_memblock = nullptr;
	if (m_expand_index_memblock)
	{
		dkMemBlockDestroy(m_expand_index_memblock);
		m_expand_index_memblock = nullptr;
	}
	if (m_sampler_memblock)
	{
		dkMemBlockDestroy(m_sampler_memblock);
		m_sampler_memblock = nullptr;
	}
	if (m_timestamp_memblock)
	{
		dkMemBlockDestroy(m_timestamp_memblock);
		m_timestamp_memblock = nullptr;
	}
	m_gpu_timing_enabled = false;
	m_accumulated_gpu_time = 0.0f;
	if (m_queue)
	{
		dkQueueDestroy(m_queue);
		m_queue = nullptr;
	}
	if (m_code_memblock)
	{
		dkMemBlockDestroy(m_code_memblock);
		m_code_memblock = nullptr;
		m_convert_shaders_ok = false;
	}
	DestroySwapchain();
	DrainMemBlockPool();
	if (m_device)
	{
		dkDeviceDestroy(m_device);
		m_device = nullptr;
	}
}

DkMemBlock GSDeviceDK::AcquireMemBlock(u32 size, u32 flags)
{
	for (size_t i = 0; i < m_memblock_pool.size(); i++)
	{
		PooledMemBlock& pooled = m_memblock_pool[i];
		if (pooled.size == size && pooled.flags == flags)
		{
			DkMemBlock block = pooled.block;
			m_memblock_pool_bytes -= pooled.size;
			pooled = m_memblock_pool.back();
			m_memblock_pool.pop_back();
			return block;
		}
	}

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, size);
	memblock_maker.flags = flags;
	return dkMemBlockCreate(&memblock_maker);
}

void GSDeviceDK::ReleaseMemBlock(DkMemBlock block, u32 size, u32 flags)
{
	if (!block)
		return;

	FrameContext& ctx = m_frames[m_frame_index];
	if (m_frame_active || ctx.fence_pending)
		ctx.retired_memblocks.push_back({block, size, flags});
	else
		PoolMemBlock({block, size, flags});
	// The bound textures are cached by pointer, which a new texture can reuse.
	m_hw_tex_valid = false;
}

void GSDeviceDK::PoolMemBlock(const PooledMemBlock& block)
{
	if (m_memblock_pool_bytes + block.size > MEMBLOCK_POOL_MAX_BYTES)
	{
		dkMemBlockDestroy(block.block);
		return;
	}

	m_memblock_pool.push_back(block);
	m_memblock_pool_bytes += block.size;
}

void GSDeviceDK::DrainMemBlockPool()
{
	for (const PooledMemBlock& pooled : m_memblock_pool)
		dkMemBlockDestroy(pooled.block);
	m_memblock_pool.clear();
	m_memblock_pool_bytes = 0;
}
#endif

bool GSDeviceDK::HasSurface() const
{
#ifdef __SWITCH__
	return m_swapchain != nullptr;
#else
	return false;
#endif
}

void GSDeviceDK::DestroySurface()
{
}

bool GSDeviceDK::UpdateWindow()
{
	return true;
}

void GSDeviceDK::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
#ifdef __SWITCH2__
	// Docking or a TV mode change resized the window: new framebuffers.
	m_window_info.surface_refresh_rate = static_cast<float>(nx2DisplayRefreshRate());
	if (!m_device || new_window_width == 0 || new_window_height == 0 ||
		(m_swapchain && static_cast<int>(new_window_width) == m_present_width &&
			static_cast<int>(new_window_height) == m_present_height))
		return;

	dkQueueWaitIdle(m_queue);
	DestroyFrameGeneration();
	DestroySwapchain();
	m_present_width = static_cast<int>(new_window_width);
	m_present_height = static_cast<int>(new_window_height);
	m_window_info.surface_width = new_window_width;
	m_window_info.surface_height = new_window_height;
	m_window_info.surface_scale = new_window_scale;
	if (!CreateSwapchain())
		Console.Error("DK3D: framebuffers for %ux%u could not be created", new_window_width, new_window_height);
#endif
}

bool GSDeviceDK::SupportsExclusiveFullscreen() const
{
	return false;
}

GSDevice::PresentResult GSDeviceDK::DoBeginPresent(bool frame_skip)
{
#ifdef __SWITCH__
#ifdef __SWITCH2__
	// A swapchain that could not be made at a resize: try again once per present.
	if (!m_swapchain && m_device && !frame_skip)
		CreateSwapchain();
#endif
	if (frame_skip || !m_swapchain)
	{
		SubmitFrame();
		return PresentResult::FrameSkipped;
	}

	// DoMerge may already have opened the frame? Otherwise we start here.
	BeginFrameIfNeeded();
	WaitForCopyEngine();

#ifdef __SWITCH2__
	UpdateFrameGeneration();
	if (m_lsfg_generator)
	{
		dkImageViewDefaults(&m_swapchain_view, &m_lsfg_display);
		m_lsfg_display_used = true;
	}
	else
#endif
	{
		// The release libdeko3d svcBreaks on any acquire failure without calling cbDebug. Record
		// whether the queue had already faulted (GPU fault/timeout) as opposed to VI refusing the
		// dequeue, since the Atmosphère report can't tell the two apart.
		if (dkQueueIsInErrorState(m_queue))
			Horizon::ExitTrace("DK3D: queue in error state before dkQueueAcquireImage (GPU fault or timeout)");

		const Common::Timer::Value acquire_start = Common::Timer::GetCurrentValue();
		m_present_slot = dkQueueAcquireImage(m_queue, m_swapchain);
		PerformanceMetrics::AccumulateGSAcquireWait(
			static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - acquire_start)));
		dkImageViewDefaults(&m_swapchain_view, &m_framebuffers[m_present_slot]);
	}

	// Clear letterbox/unwritten areas before PresentRect draws.
	dkCmdBufBindRenderTarget(m_cmdbuf, &m_swapchain_view, nullptr);
	m_hw_targets_valid = false;
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(m_present_width), static_cast<float>(m_present_height),
		0.0f, 1.0f};
	const DkScissor scissor = {0, 0, static_cast<u32>(m_present_width), static_cast<u32>(m_present_height)};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
	dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

	return PresentResult::OK;
#else
	return PresentResult::FrameSkipped;
#endif
}

#ifdef __SWITCH__
void GSDeviceDK::RenderImGui()
{
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
#ifdef __SWITCH2__
	const bool presenting = m_present_slot >= 0 || m_lsfg_display_used;
#else
	const bool presenting = m_present_slot >= 0;
#endif
	if (!draw_data || draw_data->CmdLists.Size == 0 || !m_imgui_shaders_ok || !m_frame_active || !presenting)
		return;

	UpdateImGuiTextures();

	InvalidateHWStateCache();

	static_assert(sizeof(ImDrawIdx) == sizeof(u16), "ImGui index buffer must be 16-bit");

	const float width = static_cast<float>(m_present_width);
	const float height = static_cast<float>(m_present_height);

	// Draw over the already-presented frame
	dkCmdBufBindRenderTarget(m_cmdbuf, &m_swapchain_view, nullptr);
	const DkViewport viewport = {0.0f, 0.0f, width, height, 0.0f, 1.0f};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);

	// Ignnore culling and depth
	DkRasterizerState rasterizer_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	rasterizer_state.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);

	DkColorState color_state;
	dkColorStateDefaults(&color_state);
	dkColorStateSetBlendEnable(&color_state, 0, true);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);

	DkBlendState blend_state;
	dkBlendStateDefaults(&blend_state);
	dkBlendStateSetOps(&blend_state, DkBlendOp_Add, DkBlendOp_Add);
	dkBlendStateSetFactors(&blend_state, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One,
		DkBlendFactor_Zero);
	dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);

	DkColorWriteState color_write_state;
	dkColorWriteStateDefaults(&color_write_state);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);

	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	depth_state.depthTestEnable = false;
	depth_state.depthWriteEnable = false;
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);

	const DkShader* shaders[] = {&m_imgui_vsh, &m_imgui_fsh};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	struct ImGuiConstants
	{
		float scale[2];
		float translate[2];
	} constants;
	constants.scale[0] = 2.0f / width;
	constants.scale[1] = 2.0f / height;
	constants.translate[0] = -1.0f;
	constants.translate[1] = -1.0f;
	const DkGpuAddr cb_addr = StreamUniform(&constants, sizeof(constants));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 0, cb_addr, AlignUp(sizeof(constants), DK_UNIFORM_BUF_ALIGNMENT));

	static const DkVtxAttribState attribs[3] = {
		{0, 0, offsetof(ImDrawVert, pos), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ImDrawVert, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ImDrawVert, col), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
	};
	static const DkVtxBufferState buffer_state = {sizeof(ImDrawVert), 0};
	dkCmdBufBindVtxAttribState(m_cmdbuf, attribs, 3);
	dkCmdBufBindVtxBufferState(m_cmdbuf, &buffer_state, 1);

	const GSVector4i rt_bounds = GSVector4i(0, 0, m_present_width, m_present_height);

	for (int n = 0; n < draw_data->CmdLists.Size; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];

		const u32 vtx_size = static_cast<u32>(cmd_list->VtxBuffer.Size) * sizeof(ImDrawVert);
		const u32 idx_size = static_cast<u32>(cmd_list->IdxBuffer.Size) * sizeof(ImDrawIdx);
		const DkGpuAddr vtx_addr = StreamVertices(cmd_list->VtxBuffer.Data, vtx_size);
		const DkGpuAddr idx_addr = StreamIndices(cmd_list->IdxBuffer.Data, idx_size);
		dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vtx_addr, vtx_size);
		dkCmdBufBindIdxBuffer(m_cmdbuf, DkIdxFormat_Uint16, idx_addr);

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			if (pcmd->UserCallback)
				continue;

			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			const GSVector4i iclip = GSVector4i(clip).rintersect(rt_bounds);
			if (iclip.rempty())
				continue;
			const DkScissor dk_scissor = {static_cast<u32>(iclip.x), static_cast<u32>(iclip.y),
				static_cast<u32>(iclip.width()), static_cast<u32>(iclip.height())};
			dkCmdBufSetScissors(m_cmdbuf, 0, &dk_scissor, 1);

			GSTextureDK* const tex = reinterpret_cast<GSTextureDK*>(pcmd->GetTexID());
			ReserveImageDescriptors(1);
			const u32 slot = tex ? PushImage(tex) : 0;
			const DkResHandle handle = dkMakeTextureHandle(slot, SAMPLER_LINEAR);
			dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &handle, 1);

			WaitForCopyEngine();
			dkCmdBufDrawIndexed(m_cmdbuf, DkPrimitive_Triangles, pcmd->ElemCount, 1, pcmd->IdxOffset, pcmd->VtxOffset,
				0);
		}

		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}
}
#endif

void GSDeviceDK::EndPresent()
{
#ifdef __SWITCH__
	RenderImGui();
#ifdef __SWITCH2__
	UpdateFrameGenerationDisplay();
#endif
	SubmitFrame();
#endif
}

#ifdef __SWITCH__
void GSDeviceDK::SubmitFrameCommands()
{
	FrameContext& ctx = m_frames[m_frame_index];
	if (ctx.timestamp_written)
		WriteGPUTimestamp(m_frame_index, 1);
#ifdef __SWITCH2__
	// The fence must also cover copies still running on the copy engine.
	if (m_readback_pending || m_copy_engine_busy)
#else
	if (m_readback_pending)
#endif
	{
		IssueBarrier(READBACK_BARRIER, m_readback_pending ? DkInvalidateFlags_L2Cache : 0);
		g_perfmon.Put(GSPerfMon::Barriers, 1);
		m_readback_pending = false;
	}
	dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
	dkQueueSignalFence(m_queue, &ctx.fence, false);
	ctx.fence_pending = true;
	ctx.serial = ++m_submitted_serial;
}

void GSDeviceDK::SubmitFrame()
{
	if (!m_frame_active)
		return;

#ifdef __SWITCH2__
	if (m_lsfg_display_used)
	{
		SubmitGeneratedFrame();
		return;
	}
#endif
#ifdef __SWITCH2__
	const int guest_slot = m_present_slot >= 0 ? RecordGuestCopy(m_framebuffers[m_present_slot]) : -1;
#endif
	SubmitFrameCommands();
	if (m_present_slot >= 0)
		dkQueuePresentImage(m_queue, m_swapchain, m_present_slot);
	else
		dkQueueFlush(m_queue);
#ifdef __SWITCH2__
	PresentGuestCopy(guest_slot);
#endif

	m_present_slot = -1;
	m_frame_active = false;
}

#ifdef __SWITCH2__
namespace
{
	u64 MonotonicNs()
	{
		return static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue()));
	}
} // namespace

void GSDeviceDK::UpdateFrameGeneration()
{
	const SwitchLSFGState state = SwitchLSFG_PollState();
	const Deko3DLSFG::Config config{state.performance, state.flow_scale >= 1.0f ? 1u : state.flow_scale >= 0.5f ? 2u : 4u};
	const bool quality_changed = config.performance != m_lsfg_config.performance;
	const bool changed = quality_changed || config.flowDenominator != m_lsfg_config.flowDenominator ||
		state.enabled != m_lsfg_state.enabled;
	const bool speed_changed = state.speed_control_state != m_lsfg_state.speed_control_state;
	if (changed || m_lsfg_failed)
		DestroyFrameGeneration();
	if (changed)
	{
		m_lsfg_pacer.Reset();
		m_lsfg_failed = false;
	}
	if (quality_changed || !state.enabled)
	{
		// The worker stops between modules; its result is dropped when it ends (no wait on this thread).
		if (m_lsfg_preparing)
			m_lsfg_preparation_cancel.store(true, std::memory_order_release);
		if (quality_changed)
			m_lsfg_shader_directory.clear();
	}
	m_lsfg_config = config;
	m_lsfg_state = state;
	if (state.reset_session || speed_changed)
	{
		m_lsfg_pacer.Reset();
		if (state.reset_session)
			m_lsfg_failed = false;
		if (m_lsfg_generator)
			m_lsfg_generator->Reset();
	}

	if (m_lsfg_preparing && m_lsfg_preparation_done.load(std::memory_order_acquire))
	{
		m_lsfg_preparation_thread.Join();
		m_lsfg_preparing = false;
		if (!m_lsfg_prepared.cancelled && m_lsfg_preparation_performance == config.performance)
		{
			if (m_lsfg_prepared.error.empty())
			{
				m_lsfg_shader_directory = std::move(m_lsfg_prepared.directory);
				Console.WriteLn("LSFG: shaders ready in %s", m_lsfg_shader_directory.c_str());
			}
			else if (state.enabled)
			{
				m_lsfg_failed = true;
				Console.Error("LSFG: shader preparation failed: %s", m_lsfg_prepared.error.c_str());
			}
		}
	}
	if (!state.enabled || m_lsfg_failed)
	{
		SwitchLSFG_SetStatus(!m_lsfg_failed, false, m_lsfg_preparing);
		return;
	}

	// Lossless.dll's shaders are compiled for drako3d once, then read from the cache.
	if (!m_lsfg_preparing && m_lsfg_shader_directory.empty())
	{
		m_lsfg_preparing = true;
		m_lsfg_preparation_performance = config.performance;
		m_lsfg_preparation_cancel.store(false, std::memory_order_release);
		m_lsfg_preparation_done.store(false, std::memory_order_release);
		m_lsfg_preparation_thread.SetStackSize(4 * 1024 * 1024);
		m_lsfg_preparation_thread.Start([this, performance = config.performance]() {
			Threading::SetNameOfCurrentThread("LSFG shaders");
			m_lsfg_prepared = Deko3DLSFG::PrepareShaders(
				performance, m_lsfg_preparation_cancel, SWITCH_LSFG_DLL_PATH, SWITCH_LSFG_DEKO_CACHE_ROOT);
			m_lsfg_preparation_done.store(true, std::memory_order_release);
		});
	}

	// Docked with the 120 Hz option, a 50/60 FPS game moves the output to 1440p: wait until the game is measured and the
	// window has the size it keeps, rather than make a generator for a size that is then replaced.
	u32 window_width = 0, window_height = 0;
	const bool resizing = R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &window_width, &window_height)) &&
		(static_cast<int>(window_width) != m_present_width || static_cast<int>(window_height) != m_present_height);
	const bool measuring = state.high_refresh && nx2DisplayRefreshRate() < 120 && !m_lsfg_pacer.LowRateSource();
	if (!m_lsfg_generator && (resizing || measuring))
	{
		SwitchLSFG_SetStatus(true, false, m_lsfg_preparing);
		return;
	}

	if (!m_lsfg_generator && !m_lsfg_shader_directory.empty())
	{
		Deko3DLSFG::Services services;
		services.device = m_device;
		services.scratchPerWarp = LSFG_SCRATCH_PER_WARP;
		services.allocate = [device = m_device](uint32_t bytes, uint32_t flags, const char*) {
			return dk::UniqueMemBlock{dk::MemBlockMaker{device, AlignUp(bytes, DK_MEMBLOCK_ALIGNMENT)}.setFlags(flags).create()};
		};
		// (the generator is only destroyed once the queue is idle)
		services.retire = [](dk::UniqueMemBlock) {};
		services.log = [](const std::string& message) { Console.WriteLn("%s", message.c_str()); };
		m_lsfg_generator = Deko3DLSFG::CreateFrameGenerator(std::move(services), m_lsfg_config, m_lsfg_shader_directory,
			static_cast<u32>(m_present_width), static_cast<u32>(m_present_height));

		if (m_lsfg_generator)
		{
			DkImageLayoutMaker layout_maker;
			dkImageLayoutMakerDefaults(&layout_maker, m_device);
			layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
			layout_maker.format = DkImageFormat_RGBA8_Unorm;
			layout_maker.dimensions[0] = m_present_width;
			layout_maker.dimensions[1] = m_present_height;
			DkImageLayout layout;
			dkImageLayoutInitialize(&layout, &layout_maker);
			DkMemBlockMaker memblock_maker;
			dkMemBlockMakerDefaults(&memblock_maker, m_device,
				AlignUp(static_cast<u32>(dkImageLayoutGetSize(&layout)), std::max<u32>(dkImageLayoutGetAlignment(&layout), DK_MEMBLOCK_ALIGNMENT)));
			memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
			m_lsfg_display_memblock = dkMemBlockCreate(&memblock_maker);
			if (m_lsfg_display_memblock)
				dkImageInitialize(&m_lsfg_display, &layout, m_lsfg_display_memblock, 0);
		}
		if (!m_lsfg_generator || !m_lsfg_display_memblock)
		{
			m_lsfg_generator.reset();
			if (m_lsfg_display_memblock)
			{
				dkMemBlockDestroy(m_lsfg_display_memblock);
				m_lsfg_display_memblock = nullptr;
			}
			m_lsfg_failed = true;
			Console.Error("LSFG: the frame generator could not be created at %dx%d", m_present_width, m_present_height);
		}
	}
	SwitchLSFG_SetStatus(!m_lsfg_failed, false, m_lsfg_preparing);
}

void GSDeviceDK::UpdateFrameGenerationDisplay()
{
	// Until the generator exists, the game is measured here (for the 120 Hz option's decision).
	if (!m_lsfg_display_used && m_lsfg_state.active)
		m_lsfg_pacer.Observe(MonotonicNs(), true, false);
	if (!m_lsfg_state.enabled || !m_lsfg_state.high_refresh)
		m_lsfg_high_refresh = false;
	else if (m_lsfg_state.active && m_lsfg_pacer.HighRateSource())
		m_lsfg_high_refresh = true;
	// Docked with the 120 Hz option, a 50/60 FPS game moves the output to 120 Hz (1440p or 1080p): the new size reaches
	// ResizeWindow through Host::BeginPresentFrame.
	if (nx2DisplaySetFrameGeneration(m_lsfg_high_refresh))
		nx2DisplayReapply();
	m_lsfg_pacer.SetRefreshRate(nx2DisplayRefreshRate());
}

bool GSDeviceDK::PresentFrameGenerationImage(const DkImage& image, bool last)
{
	if (dkQueueIsInErrorState(m_queue))
		return false;
	const int slot = dkQueueAcquireImage(m_queue, m_swapchain);
	if (slot < 0 || slot >= static_cast<int>(NUM_FRAMEBUFFERS))
		return false;
	DkImageView source, destination;
	dkImageViewDefaults(&source, &image);
	dkImageViewDefaults(&destination, &m_framebuffers[slot]);
	const DkImageRect rect = {0, 0, 0, static_cast<u32>(m_present_width), static_cast<u32>(m_present_height), 1};
	IssueBarrier(DkBarrier_Full, DkInvalidateFlags_Image);
	dkCmdBufBlitImage(m_cmdbuf, &source, &rect, &destination, &rect, DkBlitFlag_FilterNearest, 0);
	m_copy_engine_busy = true;
	if (last)
		SubmitFrameCommands();
	else
		dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
	dkQueuePresentImage(m_queue, m_swapchain, slot);
	return true;
}

void GSDeviceDK::SubmitGeneratedFrame()
{
	m_lsfg_display_used = false;
	const auto decision = m_lsfg_pacer.Observe(MonotonicNs(), m_lsfg_state.active, m_lsfg_generator != nullptr);
	if (decision.reset && m_lsfg_generator)
		m_lsfg_generator->Reset();
	bool generated = false;
	// Only a present that carries a new game frame is interpolated.
	if (decision.generate && m_lsfg_generator && m_present_has_new_frame)
	{
		generated = m_lsfg_generator->Generate(m_cmdbuf, m_lsfg_display, static_cast<u32>(m_present_width),
			static_cast<u32>(m_present_height));
		m_lsfg_failed = m_lsfg_generator->Failed();
		// (the generator bound its own shaders, descriptors and images)
		InvalidateHWStateCache();
	}
	SwitchLSFG_SetStatus(!m_lsfg_failed, decision.highRate, m_lsfg_preparing);
	nx2DisplayApply(generated || m_vsync_mode != GSVSyncMode::Disabled ? 1 : 0);

	// (the guest gets the real frame only)
	const int guest_slot = RecordGuestCopy(m_lsfg_display);
	const u64 present_start = MonotonicNs();
	bool presented = !generated || PresentFrameGenerationImage(*m_lsfg_generator->GeneratedImage(), false);
	presented = presented && PresentFrameGenerationImage(m_lsfg_display, true);
	m_lsfg_pacer.PresentTime(MonotonicNs() - present_start);
	if (!presented)
	{
		// No image to present to (or a fault): the frame's work still goes to the GPU.
		SubmitFrameCommands();
		dkQueueFlush(m_queue);
	}
	PresentGuestCopy(guest_slot);
	m_present_slot = -1;
	m_frame_active = false;
}

bool GSDeviceDK::OpenGuestScreen(void* window)
{
	CloseGuestScreen();
	NWindow* const guest_window = static_cast<NWindow*>(window);
	if (!m_device || !guest_window || R_FAILED(nwindowGetDimensions(guest_window, &m_guest_width, &m_guest_height)) ||
		!m_guest_width || !m_guest_height)
		return false;

	DkImageLayoutMaker layout_maker;
	dkImageLayoutMakerDefaults(&layout_maker, m_device);
	layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
	layout_maker.format = DkImageFormat_RGBA8_Unorm;
	layout_maker.dimensions[0] = m_guest_width;
	layout_maker.dimensions[1] = m_guest_height;
	DkImageLayout layout;
	dkImageLayoutInitialize(&layout, &layout_maker);
	const u32 align = dkImageLayoutGetAlignment(&layout);
	const u32 size = (static_cast<u32>(dkImageLayoutGetSize(&layout)) + align - 1) & ~(align - 1);

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, NUM_GUEST_FRAMEBUFFERS * size);
	memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	m_guest_memblock = dkMemBlockCreate(&memblock_maker);
	if (!m_guest_memblock)
		return false;
	const DkImage* images[NUM_GUEST_FRAMEBUFFERS];
	for (u32 i = 0; i < NUM_GUEST_FRAMEBUFFERS; i++)
	{
		dkImageInitialize(&m_guest_framebuffers[i], &layout, m_guest_memblock, i * size);
		images[i] = &m_guest_framebuffers[i];
	}
	// (the guest's window paces itself: no nx2 display pacing here)
	DkSwapchainMaker swapchain_maker;
	dkSwapchainMakerDefaults(&swapchain_maker, m_device, guest_window, images, NUM_GUEST_FRAMEBUFFERS);
	m_guest_swapchain = dkSwapchainCreate(&swapchain_maker);
	if (!m_guest_swapchain)
	{
		CloseGuestScreen();
		return false;
	}
	Console.WriteLn("DK3D: GameShare guest screen %ux%u.", m_guest_width, m_guest_height);
	return true;
}

void GSDeviceDK::CloseGuestScreen()
{
	if (!m_guest_swapchain && !m_guest_memblock)
		return;
	if (m_queue)
		dkQueueWaitIdle(m_queue);
	if (m_guest_swapchain)
	{
		dkSwapchainDestroy(m_guest_swapchain);
		m_guest_swapchain = nullptr;
	}
	if (m_guest_memblock)
	{
		dkMemBlockDestroy(m_guest_memblock);
		m_guest_memblock = nullptr;
	}
}

int GSDeviceDK::RecordGuestCopy(const DkImage& source)
{
	if (!m_guest_swapchain || dkQueueIsInErrorState(m_queue))
		return -1;
	const int slot = dkQueueAcquireImage(m_queue, m_guest_swapchain);
	if (slot < 0 || slot >= static_cast<int>(NUM_GUEST_FRAMEBUFFERS))
		return -1;
	DkImageView source_view, destination;
	dkImageViewDefaults(&source_view, &source);
	dkImageViewDefaults(&destination, &m_guest_framebuffers[slot]);
	const DkImageRect source_rect = {0, 0, 0, static_cast<u32>(m_present_width), static_cast<u32>(m_present_height), 1};
	const DkImageRect guest_rect = {0, 0, 0, m_guest_width, m_guest_height, 1};
	IssueBarrier(DkBarrier_Full, DkInvalidateFlags_Image);
	dkCmdBufBlitImage(m_cmdbuf, &source_view, &source_rect, &destination, &guest_rect, DkBlitFlag_FilterLinear, 0);
	m_copy_engine_busy = true;
	return slot;
}

void GSDeviceDK::PresentGuestCopy(int slot)
{
	if (slot >= 0 && m_guest_swapchain)
		dkQueuePresentImage(m_queue, m_guest_swapchain, slot);
}

void GSDeviceDK::DestroyFrameGeneration()
{
	if (!m_lsfg_generator && !m_lsfg_display_memblock)
		return;
	dkQueueWaitIdle(m_queue);
	m_lsfg_generator.reset();
	if (m_lsfg_display_memblock)
	{
		dkMemBlockDestroy(m_lsfg_display_memblock);
		m_lsfg_display_memblock = nullptr;
	}
	m_lsfg_display = {};
	m_lsfg_display_used = false;
	InvalidateHWStateCache();
}

void GSDeviceDK::StopFrameGenerationPreparation()
{
	if (!m_lsfg_preparing)
		return;
	m_lsfg_preparation_cancel.store(true, std::memory_order_release);
	m_lsfg_preparation_thread.Join();
	m_lsfg_preparing = false;
}
#endif
#endif

void GSDeviceDK::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
#ifdef __SWITCH2__
	// drako3d presents at the default window's interval; at 120 Hz nx2 paces with VRR instead.
	nx2DisplayApply(mode == GSVSyncMode::Disabled ? 0 : 1);
#endif
}

std::string GSDeviceDK::GetDriverInfo() const
{
#ifdef __SWITCH2__
	return "drako3d";
#else
	return "deko3d";
#endif
}

bool GSDeviceDK::SetGPUTimingEnabled(bool enabled)
{
#ifdef __SWITCH__
	// Timing needs the report buffer
	m_gpu_timing_enabled = enabled && (m_timestamp_memblock != nullptr);
	if (!m_gpu_timing_enabled)
		m_accumulated_gpu_time = 0.0f;
	return (enabled == m_gpu_timing_enabled);
#else
	return false;
#endif
}

float GSDeviceDK::GetAndResetAccumulatedGPUTime()
{
#ifdef __SWITCH__
	const float time = m_accumulated_gpu_time;
	m_accumulated_gpu_time = 0.0f;
	return time;
#else
	return 0.0f;
#endif
}

void GSDeviceDK::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceDK::PopDebugGroup()
{
}

void GSDeviceDK::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

std::unique_ptr<GSDownloadTexture> GSDeviceDK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
#ifdef __SWITCH__
	return GSDownloadTextureDK::Create(this, width, height, format);
#else
	return nullptr;
#endif
}

#ifdef __SWITCH__
void GSDeviceDK::ReadbackTexture(GSTextureDK* src, const GSVector4i& rect, u32 src_level,
	DkMemBlock dst_block, u32 dst_offset, u32 dst_pitch)
{
	if (!src || rect.rempty() || rect.left < 0 || rect.top < 0)
		return;
	// The copy engine reads exactly this rectangle: keep it inside the level.
	const int right = std::min(rect.right, std::max(1, src->GetWidth() >> src_level));
	const int bottom = std::min(rect.bottom, std::max(1, src->GetHeight() >> src_level));
	if (right <= rect.left || bottom <= rect.top)
		return;

	// Preserve current frame work
	BeginFrameIfNeeded();
	InvalidateHWStateCache();
	CommitClear(src);

	DkImageView src_view;
	src->GetImageView(&src_view);
	src_view.mipLevelOffset = static_cast<uint8_t>(src_level);
	src_view.mipLevelCount = 1;
	const DkImageRect src_rect = {static_cast<u32>(rect.left), static_cast<u32>(rect.top), 0,
		static_cast<u32>(right - rect.left), static_cast<u32>(bottom - rect.top), 1};

	const DkCopyBuf dst = {dkMemBlockGetGpuAddr(dst_block) + dst_offset, dst_pitch, 0};
	BeforeCopyEngine();
	dkCmdBufCopyImageToBuffer(m_cmdbuf, &src_view, &src_rect, &dst, 0);

	// Copy now rides in the frame command buffer
	m_readback_pending = true;
}

void GSDeviceDK::FlushReadback()
{
	if (!m_readback_pending)
		return;
	m_readback_pending = false;

	BeginFrameIfNeeded();

	// Finish this list so EndPresent won't resubmit it, then wait only on the copy, not the entire frame
	IssueBarrier(READBACK_BARRIER, DkInvalidateFlags_L2Cache);
	g_perfmon.Put(GSPerfMon::Barriers, 1);
	dkQueueSubmitCommands(m_queue, dkCmdBufFinishList(m_cmdbuf));
	dkQueueSignalFence(m_queue, &m_readback_fence, false);
	dkQueueFlush(m_queue);
	m_submitted_serial++;
	const Common::Timer::Value wait_start = Common::Timer::GetCurrentValue();
	dkFenceWait(&m_readback_fence, -1);
	m_completed_serial = m_submitted_serial;
	PerformanceMetrics::AccumulateGSGpuWait(
		static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - wait_start)));
	// The submitted part may have cached the headers of the descriptor slots written next.
	IssueBarrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
}

bool GSDeviceDK::IsSubmissionComplete(u64 serial)
{
	if (serial <= m_completed_serial)
		return true;
	if (serial > m_submitted_serial)
		return false;

	for (FrameContext& ctx : m_frames)
	{
		if (ctx.fence_pending && ctx.serial >= serial && dkFenceWait(&ctx.fence, 0) == DkResult_Success)
		{
			m_completed_serial = std::max(m_completed_serial, ctx.serial);
			return true;
		}
	}
	return false;
}

void GSDeviceDK::WaitForSubmission(u64 serial)
{
	if (IsSubmissionComplete(serial))
		return;

	if (serial > m_submitted_serial)
	{
		FlushReadback();
		return;
	}

	FrameContext* oldest = nullptr;
	for (FrameContext& ctx : m_frames)
	{
		if (ctx.fence_pending && ctx.serial >= serial && (!oldest || ctx.serial < oldest->serial))
			oldest = &ctx;
	}
	if (!oldest)
		return;

	const Common::Timer::Value wait_start = Common::Timer::GetCurrentValue();
	dkFenceWait(&oldest->fence, -1);
	PerformanceMetrics::AccumulateGSGpuWait(
		static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - wait_start)));
	m_completed_serial = std::max(m_completed_serial, oldest->serial);
}

bool GSDeviceDK::UploadToImage(GSTextureDK* dst_tex, const DkImageView& view, const DkImageRect& rect, const void* data,
	u32 src_pitch, u32 upload_pitch, u32 num_rows)
{
	const u32 upload_size = num_rows * upload_pitch;
	if (upload_size == 0)
		return false;
	BeginFrameIfNeeded();
	CommitClear(dst_tex);

	DkGpuAddr src_addr;
	u8* const dst = static_cast<u8*>(ReserveStreamMemory(m_staging_memblock, m_staging_offset, upload_size,
		STAGING_ALIGNMENT, STAGING_BUFFER_SIZE, &src_addr));

	const u8* const src = static_cast<const u8*>(data);
	const u32 copy_bytes = std::min<u32>(upload_pitch, src_pitch);
	for (u32 y = 0; y < num_rows; ++y)
		std::memcpy(dst + y * upload_pitch, src + y * src_pitch, copy_bytes);

	const DkCopyBuf copy_src = {src_addr, 0, 0};
	BeforeCopyEngine();
	dkCmdBufCopyBufferToImage(m_cmdbuf, &copy_src, &view, &rect, 0);
	// Don't drain the pipeline per upload
	if (dst_tex)
		dst_tex->SetWriteGen(0);

	g_perfmon.Put(GSPerfMon::TextureUploads, 1);
	return true;
}

void GSDeviceDK::GenerateImageMipmaps(GSTextureDK* tex, DkImage* image, int width, int height, int levels)
{
	if (levels <= 1)
		return;

	BeginFrameIfNeeded();
	CommitClear(tex);

	BeforeCopyEngine();
	for (int level = 1; level < levels; level++)
	{
		DkImageView src_view;
		dkImageViewDefaults(&src_view, image);
		src_view.mipLevelOffset = static_cast<uint8_t>(level - 1);
		src_view.mipLevelCount = 1;

		DkImageView dst_view;
		dkImageViewDefaults(&dst_view, image);
		dst_view.mipLevelOffset = static_cast<uint8_t>(level);
		dst_view.mipLevelCount = 1;

		const DkImageRect src_rect = {0, 0, 0, static_cast<u32>(std::max(1, width >> (level - 1))),
			static_cast<u32>(std::max(1, height >> (level - 1))), 1};
		const DkImageRect dst_rect = {0, 0, 0, static_cast<u32>(std::max(1, width >> level)),
			static_cast<u32>(std::max(1, height >> level)), 1};

		dkCmdBufBlitImage(m_cmdbuf, &src_view, &src_rect, &dst_view, &dst_rect, DkBlitFlag_FilterLinear, 0);
	}
	if (tex)
		tex->SetWriteGen(0);
}
#endif

void GSDeviceDK::DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
#ifdef __SWITCH__
	if (!sTex || !dTex)
		return;

	GSTextureDK* const src = static_cast<GSTextureDK*>(sTex);
	GSTextureDK* const dst = static_cast<GSTextureDK*>(dTex);

	// The copy engine reads and writes exactly this rectangle: keep it inside both images.
	const GSVector4i sr = r.rintersect(GSVector4i(0, 0, src->GetWidth(), src->GetHeight()));
	const u32 dx = destX + static_cast<u32>(sr.left - r.left);
	const u32 dy = destY + static_cast<u32>(sr.top - r.top);
	const int width = std::min(sr.width(), dst->GetWidth() - static_cast<int>(dx));
	const int height = std::min(sr.height(), dst->GetHeight() - static_cast<int>(dy));
	if (sr.rempty() || width <= 0 || height <= 0)
		return;

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	BeginFrameIfNeeded();
	// dkCmdBufCopyImage may reprogram 3D-engine state
	InvalidateHWStateCache();
	CommitClear(src);
	CommitClear(dst);

	DkImageView src_view;
	DkImageView dst_view;
	src->GetImageView(&src_view);
	dst->GetImageView(&dst_view);

	const DkImageRect src_rect = {static_cast<u32>(sr.left), static_cast<u32>(sr.top), 0, static_cast<u32>(width),
		static_cast<u32>(height), 1};
	const DkImageRect dst_rect = {dx, dy, 0, static_cast<u32>(width), static_cast<u32>(height), 1};
	BeforeCopyEngine();
	dkCmdBufCopyImage(m_cmdbuf, &src_view, &src_rect, &dst_view, &dst_rect, 0);

	dst->SetState(GSTexture::State::Dirty);
	dst->SetWriteGen(m_gpu_write_gen);
#endif
}

void GSDeviceDK::DoConvert(GSTextureDK* sTex, const GSVector4& sRect, GSTextureDK* dTex, const GSVector4& dRect,
	ShaderConvertSelector selector, bool linear)
{
#ifdef __SWITCH__
	const ShaderConvert shader = selector.Shader();
	const u32 color_write_mask = selector.Mask() & selector.DefaultMask();
	// COPY uses the plain copy shader
	if (shader == ShaderConvert::COPY)
	{
		DoStretchRectImpl(sTex, sRect, dTex, dRect, &m_copy_fsh, linear, nullptr, 0, false, color_write_mask);
		return;
	}

	struct
	{
		u32 variant;
		u32 bilinear;
		u32 pad[2];
	} ub = {static_cast<u32>(shader), selector.Biln() ? 1u : 0u, {0, 0}};

	if (IsIntegerOutput(shader))
	{
		DoStretchRectImpl(sTex, sRect, dTex, dRect, &m_convert_int_fsh, false, &ub, sizeof(ub), false,
			color_write_mask);
		return;
	}

	DoStretchRectImpl(sTex, sRect, dTex, dRect, &m_convert_fsh, linear, &ub, sizeof(ub), selector.DepthOutput(),
		color_write_mask);
#endif
}

void GSDeviceDK::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
#ifdef __SWITCH__
	DoConvert(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, shader,
		filter == Filter::Biln);
#endif
}

void GSDeviceDK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
#ifdef __SWITCH__
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect, &m_copy_fsh,
		filter == Filter::Biln);
#endif
}

void GSDeviceDK::DoUpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
#ifdef __SWITCH__
	if (!m_texconvert_shaders_ok)
		return;

	struct
	{
		u32 offset[2];
		u32 doffset;
		u32 variant; // 0 = CLUT_4, 1 = CLUT_8
		float scale;
		float pad[3];
	} ub = {{offsetX, offsetY}, dOffset, (dSize == 16) ? 0u : 1u, sScale, {}};

	const GSVector4 dRect(0.0f, 0.0f, static_cast<float>(dSize), 1.0f);
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4::zero(), static_cast<GSTextureDK*>(dTex), dRect,
		&m_clut_fsh, false, &ub, sizeof(ub));
#endif
}

void GSDeviceDK::DoConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
	GSTexture* dTex, u32 DBW, u32 DPSM)
{
#ifdef __SWITCH__
	if (!m_texconvert_shaders_ok)
		return;

	struct
	{
		u32 SBW;
		u32 DBW;
		u32 PSM;
		u32 variant; // 0 = RGBA_TO_8I, 1 = RGB5A1_TO_8I
		float ScaleFactor;
		float pad[3];
	} ub = {SBW, DBW, SPSM, ((SPSM & 0xE) == 0) ? 0u : 1u, sScale, {}};

	const GSVector4 dRect(0.0f, 0.0f, static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4::zero(), static_cast<GSTextureDK*>(dTex), dRect,
		&m_convert_8i_fsh, false, &ub, sizeof(ub));
#endif
}

void GSDeviceDK::DoFilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
#ifdef __SWITCH__
	if (!m_texconvert_shaders_ok)
		return;

	struct
	{
		s32 ClampMin[2];
		s32 DownsampleFactor;
		s32 pad0;
		float Weight;
		float pad1[3];
	} ub = {{clamp_min.x, clamp_min.y}, static_cast<s32>(downsample_factor), 0,
		static_cast<float>(downsample_factor * downsample_factor), {}};

	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4::zero(), static_cast<GSTextureDK*>(dTex), dRect,
		&m_downsample_fsh, false, &ub, sizeof(ub));
#endif
}

// Pick the cheapest tfx fragment variant a draw can use.
u32 GSDeviceDK::SelectTfxVariant(const GSHWDrawConfig& config)
{
#ifdef __SWITCH__
	const GSHWDrawConfig::PSSelector& ps = config.ps;

	// Heavy per-pixel effects the specialised variants fold out entirely
	const bool special = (ps.channel != 0 || ps.depth_fmt != 0 ||
		ps.urban_chaos_hle || ps.tales_of_abyss_hle || ps.shuffle || ps.dither ||
		ps.quantize_color || ps.substitute_alpha);

	const bool extra_pass = config.blend_multi_pass.enable || config.alpha_second_pass.enable;

	const bool simple_sample = (ps.ltf == 0 && ps.aem_fmt == 0 &&
		ps.region_rect == 0 && ps.adjs == 0 && ps.adjt == 0 && ps.automatic_lod == 0 &&
		ps.manual_lod == 0 && ps.wms < 2 && ps.wmt < 2 &&
		ps.replacement_alpha_snap == 0 && ps.native_texel_grid == 0);

	// Feedback draws bind RtSampler, so they can use the feedback variants
	if (config.require_one_barrier || config.require_full_barrier)
	{
		if (special || extra_pass)
			return TfxVariantUber;
		if (simple_sample)
			return (ps.pal_fmt == 0) ? TfxVariantFeedbackFast : TfxVariantFeedbackPalette;
		return TfxVariantFeedback;
	}

	if (extra_pass)
		return TfxVariantUber;

	if (ps.IsFeedbackLoopRT())
		return TfxVariantUber;

	// shader-side blend, fbmask, tex-is-fb, DATE all read the target.
	if (ps.blend_a || ps.blend_b || ps.blend_d || ps.blend_hw || ps.a_masked ||
		ps.tex_is_fb || ps.fbmask || ps.date != 0)
		return TfxVariantUber;

	if (special)
		return TfxVariantUber;

	if (simple_sample)
		return (ps.pal_fmt == 0) ? TfxVariantFast : TfxVariantPalette;
	return TfxVariantOpaque;
#else
	return TfxVariantUber;
#endif
}

void GSDeviceDK::DoRenderHW(GSHWDrawConfig& config)
{
#ifdef __SWITCH__
	if (!m_tfx_shaders_ok)
		return;

	GSTextureDK* const rt = static_cast<GSTextureDK*>(config.rt);
	GSTextureDK* const ds = static_cast<GSTextureDK*>(config.ds);
	const bool has_ds = (ds && ds->IsDepth());
	if ((!rt && !has_ds) || (rt && rt->IsDepth()) || config.nindices == 0 || !config.verts || !config.indices)
		return;
	if (static_cast<u32>(config.vs.expand) >= NUM_TFX_VS ||
		(config.vs.expand != GSHWDrawConfig::VSExpand::None && !m_features.vs_expand))
	{
		pxFail("DK3D: unsupported vertex shader expansion");
		return;
	}

	GSTextureDK* const tex = static_cast<GSTextureDK*>(config.tex);
	GSTextureDK* const pal = static_cast<GSTextureDK*>(config.pal);

	BeginFrameIfNeeded();
	WaitForCopyEngine();

	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	CommitClear(rt);
	CommitClear(ds);
	CommitClear(tex);
	CommitClear(pal);
	if ((tex && tex->GetWriteGen() == m_gpu_write_gen) ||
		(pal && pal->GetWriteGen() == m_gpu_write_gen))
	{
		IssueBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
		g_perfmon.Put(GSPerfMon::Barriers, 1);
	}

	// Destination-alpha setup
	GSTextureDK* date_image = nullptr;
	if (rt && config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil)
	{
		SetupDATE(rt, ds, config.datm, config.drawarea);
	}
	else if (rt && config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		date_image = SetupPrimitiveTrackingDATE(config);
		if (!date_image)
			return; // allocation faild
	}

	const GSVector2i rtsize = rt ? rt->GetSize() : ds->GetSize();
	const u64 rt_uid = rt ? rt->GetUID() : 0;
	const u64 ds_uid = has_ds ? ds->GetUID() : 0;
	if (!m_hw_targets_valid || m_hw_last_rt_uid != rt_uid || m_hw_last_ds_uid != ds_uid)
	{
		DkImageView rt_view;
		DkImageView ds_view;
		if (rt)
			rt->GetImageView(&rt_view);
		if (has_ds)
			ds->GetImageView(&ds_view);
		if (rt)
			dkCmdBufBindRenderTarget(m_cmdbuf, &rt_view, has_ds ? &ds_view : nullptr);
		else
			dkCmdBufBindRenderTargets(m_cmdbuf, nullptr, 0, &ds_view);

		const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(rtsize.x), static_cast<float>(rtsize.y), 0.0f, 1.0f};
		dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);

		m_hw_last_rt_uid = rt_uid;
		m_hw_last_ds_uid = ds_uid;
		m_hw_targets_valid = true;
	}

	const GSVector4i scissor = config.scissor.rintersect(GSVector4i::loadh(rtsize));
	const DkScissor dk_scissor = {static_cast<u32>(std::max(0, scissor.x)), static_cast<u32>(std::max(0, scissor.y)),
		static_cast<u32>(std::max(0, scissor.width())), static_cast<u32>(std::max(0, scissor.height()))};
	dkCmdBufSetScissors(m_cmdbuf, 0, &dk_scissor, 1);

	if (rt)
		rt->SetState(GSTexture::State::Dirty);
	if (has_ds)
	{
		ds->SetState(GSTexture::State::Dirty);

		if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
			dkCmdBufClearDepthStencil(m_cmdbuf, false, 0.0f, 0xFF, 1);
	}

	// Route to the cheapest specialised fragment shader this draw can use.
	const u32 tfx_variant = SelectTfxVariant(config);
	const GSHWDrawConfig::VSExpand vs_expand = config.vs.expand;
	ReserveImageDescriptors(4);

	const bool force_state =
		!m_hw_invariants_bound || m_hw_tfx_variant != tfx_variant || m_hw_vs_expand != vs_expand;
	if (force_state)
	{
		DkRasterizerState rasterizer_state;
		dkRasterizerStateDefaults(&rasterizer_state);
		rasterizer_state.cullMode = DkFace_None;
		dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);

		const DkShader* shaders[] = {&m_tfx_vsh[static_cast<u32>(vs_expand)], &m_tfx_fsh[tfx_variant]};
		dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

		dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
		dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

		BindTfxVertexInput(m_cmdbuf, vs_expand);

		m_hw_invariants_bound = true;
		m_hw_tfx_variant = tfx_variant;
		m_hw_vs_expand = vs_expand;
	}

	// Per-draw state is only re-bound when it differs from the previous draw.
	auto bind_blend = [&](const GSHWDrawConfig::BlendState& blend, bool force) {
		if (!force && m_hw_blend_key == blend.key)
			return;
		m_hw_blend_key = blend.key;

		DkColorState color_state;
		dkColorStateDefaults(&color_state);
		DkBlendState blend_state;
		dkBlendStateDefaults(&blend_state);
		if (blend.enable)
		{
			dkColorStateSetBlendEnable(&color_state, 0, true);
			dkBlendStateSetOps(&blend_state, kBlendOps[blend.op], DkBlendOp_Add);
			dkBlendStateSetFactors(&blend_state, kBlendFactors[blend.src_factor],
				kBlendFactors[blend.dst_factor], kBlendFactors[blend.src_factor_alpha],
				kBlendFactors[blend.dst_factor_alpha]);
			if (blend.constant_enable)
			{
				const float c = static_cast<float>(blend.constant) / 128.0f;
				dkCmdBufSetBlendConst(m_cmdbuf, c, c, c, c);
			}
		}
		dkCmdBufBindColorState(m_cmdbuf, &color_state);
		dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);
	};

	auto bind_color_mask = [&](u32 wrgba, bool force) {
		if (!force && m_hw_colormask == wrgba)
			return;
		m_hw_colormask = wrgba;
		DkColorWriteState color_write_state;
		dkColorWriteStateDefaults(&color_write_state);
		dkColorWriteStateSetMask(&color_write_state, 0, wrgba);
		dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);
	};

	auto bind_depth = [&](const GSHWDrawConfig::DepthStencilSelector& depth, bool force) {
		// ztst/zwe plus the stencil DATE bits affect the state.
		const u32 depth_key = has_ds ? ((static_cast<u32>(depth.key) & 0x1Fu) | 0x100u) : 0u;
		if (!force && m_hw_depth_key == depth_key)
			return;
		m_hw_depth_key = depth_key;

		DkDepthStencilState depth_state;
		dkDepthStencilStateDefaults(&depth_state);
		if (has_ds)
		{
			static const DkCompareOp ztst[] = {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Gequal,
				DkCompareOp_Greater};
			depth_state.depthTestEnable = (depth.ztst != ZTST_ALWAYS || depth.zwe);
			depth_state.depthWriteEnable = depth.zwe;
			depth_state.depthCompareOp = ztst[depth.ztst];

			// Only pixels whose stencil == 1 survive
			if (depth.date)
			{
				depth_state.stencilTestEnable = true;
				depth_state.stencilFrontCompareOp = DkCompareOp_Equal;
				depth_state.stencilFrontFailOp = DkStencilOp_Keep;
				depth_state.stencilFrontDepthFailOp = DkStencilOp_Keep;
				depth_state.stencilFrontPassOp = depth.date_one ? DkStencilOp_Zero : DkStencilOp_Keep;
				depth_state.stencilBackCompareOp = DkCompareOp_Equal;
				depth_state.stencilBackFailOp = DkStencilOp_Keep;
				depth_state.stencilBackDepthFailOp = DkStencilOp_Keep;
				depth_state.stencilBackPassOp = depth.date_one ? DkStencilOp_Zero : DkStencilOp_Keep;
				dkCmdBufSetStencil(m_cmdbuf, DkFace_FrontAndBack, 1, 1, 1);
			}
		}
		else
		{
			depth_state.depthTestEnable = false;
			depth_state.depthWriteEnable = false;
		}
		dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);
	};

	bind_blend(config.blend, force_state);
	bind_color_mask(config.colormask.wrgba, force_state);
	bind_depth(config.depth, force_state);

	const u32 sampler_slot = (config.sampler.biln ? SAMPLER_LINEAR : SAMPLER_POINT) |
							 (config.sampler.tau ? 2u : 0u) | (config.sampler.tav ? 1u : 0u);
	// Reuse the prior draw's tex/pal binding when nothing changed
	if (!m_hw_tex_valid || m_hw_last_tex != tex || m_hw_last_pal != pal || m_hw_last_sampler != sampler_slot)
	{
		DkResHandle handles[2];
		handles[0] = tex ? dkMakeTextureHandle(PushImage(tex), sampler_slot) : dkMakeTextureHandle(0, SAMPLER_POINT);
		handles[1] = pal ? dkMakeTextureHandle(PushImage(pal), SAMPLER_POINT) : dkMakeTextureHandle(0, SAMPLER_POINT);
		dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 2, handles, 2);

		m_hw_last_tex = tex;
		m_hw_last_pal = pal;
		m_hw_last_sampler = sampler_slot;
		m_hw_tex_valid = true;
	}

	// Bind RT as RtSampler when shader-side blending/fbmask/DATE needs Cd/Ad.
	const bool needs_rt_tex = rt && (config.require_one_barrier || config.require_full_barrier ||
		config.alpha_second_pass.require_one_barrier || config.alpha_second_pass.require_full_barrier);
	if (needs_rt_tex)
	{
		const DkResHandle rt_handle = dkMakeTextureHandle(PushImage(rt), SAMPLER_POINT);
		dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 4, &rt_handle, 1);
	}

	// Bind the per-pixel minimum primitive id image (DATE 3)
	if (date_image)
	{
		const DkResHandle primid_handle = dkMakeTextureHandle(PushImage(date_image), SAMPLER_POINT);
		dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 5, &primid_handle, 1);
	}

	// Rebuild the selector from PSSelector so second passes can swap overrides.
	auto make_selector = [&](const GSHWDrawConfig::PSSelector& ps) { return MakeTfxSelector(ps, tex != nullptr); };

	auto bind_selector = [&](const DKTfxSelector& sel) {
		const DkGpuAddr sel_addr = StreamUniform(&sel, sizeof(sel));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 0, sel_addr, AlignUp(sizeof(sel), DK_UNIFORM_BUF_ALIGNMENT));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 0, sel_addr, AlignUp(sizeof(sel), DK_UNIFORM_BUF_ALIGNMENT));
	};
	auto bind_ps_cb = [&]() {
		const DkGpuAddr ps_addr = StreamUniform(&config.cb_ps, sizeof(config.cb_ps));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 1, ps_addr,
			AlignUp(sizeof(config.cb_ps), DK_UNIFORM_BUF_ALIGNMENT));
	};

	// Skip drawing the bindings if all three are the same as before
	const bool has_tex = (tex != nullptr);
	const bool uniforms_changed = !m_hw_uniforms_valid || m_hw_last_vs != config.cb_vs ||
								  m_hw_last_ps != config.cb_ps || m_hw_last_pssel != config.ps ||
								  m_hw_last_has_tex != has_tex;
	if (uniforms_changed)
	{
		const DkGpuAddr vs_addr = StreamUniform(&config.cb_vs, sizeof(config.cb_vs));
		dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 1, vs_addr,
			AlignUp(sizeof(config.cb_vs), DK_UNIFORM_BUF_ALIGNMENT));
		bind_ps_cb();
		bind_selector(make_selector(config.ps));

		m_hw_last_vs = config.cb_vs;
		m_hw_last_ps = config.cb_ps;
		m_hw_last_pssel = config.ps;
		m_hw_last_has_tex = has_tex;
		m_hw_uniforms_valid = true;
	}

	BindHWGeometry(config);
	const DkPrimitive primitive = GetTfxPrimitive(config.topology);

	SendHWDraw(config, primitive, config.require_one_barrier, config.require_full_barrier);

	// Blend multi-pass redraws with selector overrides
	if (config.blend_multi_pass.enable)
	{
		bind_blend(config.blend_multi_pass.blend, false);
		DKTfxSelector mp_sel = make_selector(config.ps);
		mp_sel.blend_hw = config.blend_multi_pass.blend_hw;
		mp_sel.dither = config.blend_multi_pass.dither;
		bind_selector(mp_sel);
		WaitForCopyEngine();
		dkCmdBufDrawIndexed(m_cmdbuf, primitive, config.nindices, 1, 0, 0, 0);
		g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	}

	// Alpha pass swaps PS selector, colour mask, depth, and restores original blend.
	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			bind_ps_cb();
		}
		bind_color_mask(config.alpha_second_pass.colormask.wrgba, false);
		bind_depth(config.alpha_second_pass.depth, false);
		bind_blend(config.blend, false);
		bind_selector(make_selector(config.alpha_second_pass.ps));
		SendHWDraw(config, primitive, config.alpha_second_pass.require_one_barrier,
			config.alpha_second_pass.require_full_barrier);
	}

	if (config.blend_multi_pass.enable || config.alpha_second_pass.enable)
		m_hw_uniforms_valid = false;

	// Mark the targets written this generation
	if (rt)
		rt->SetWriteGen(m_gpu_write_gen);
	if (has_ds)
		ds->SetWriteGen(m_gpu_write_gen);

	// The PrimID tracking image was only needed here
	if (date_image)
		Recycle(date_image);
#endif
}

#ifdef __SWITCH__
void GSDeviceDK::SetupDATE(GSTextureDK* rt, GSTextureDK* ds, SetDATM datm, const GSVector4i& bbox)
{
	if (!rt || !ds || !ds->IsDepth() || !m_date_shaders_ok)
		return;

	BeginFrameIfNeeded();
	WaitForCopyEngine();
	InvalidateHWStateCache();

	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	// Make the RT's prior writes visible
	CommitClear(rt);
	IssueBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
	g_perfmon.Put(GSPerfMon::Barriers, 1);

	// Bind the depth-stencil as the only target (no colour)
	DkImageView ds_view;
	ds->GetImageView(&ds_view);
	dkCmdBufBindRenderTargets(m_cmdbuf, nullptr, 0, &ds_view);

	const GSVector2i dssize = ds->GetSize();
	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(dssize.x), static_cast<float>(dssize.y), 0.0f, 1.0f};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	const DkScissor scissor = {0, 0, static_cast<u32>(dssize.x), static_cast<u32>(dssize.y)};
	dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);

	// Clear stencil to 0
	if (ds->GetState() == GSTexture::State::Cleared)
		dkCmdBufClearDepthStencil(m_cmdbuf, true, ds->GetClearDepth(), 0xFF, 0);
	else
		dkCmdBufClearDepthStencil(m_cmdbuf, false, 0.0f, 0xFF, 0);
	ds->SetState(GSTexture::State::Dirty);

	// Cull none, no colour, stencil replace-with-1 on pass.
	DkRasterizerState rasterizer_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	rasterizer_state.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);

	DkColorWriteState color_write_state;
	dkColorWriteStateDefaults(&color_write_state);
	dkColorWriteStateSetMask(&color_write_state, 0, 0);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);

	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	depth_state.depthTestEnable = false;
	depth_state.depthWriteEnable = false;
	depth_state.stencilTestEnable = true;
	depth_state.stencilFrontCompareOp = DkCompareOp_Always;
	depth_state.stencilFrontFailOp = DkStencilOp_Keep;
	depth_state.stencilFrontDepthFailOp = DkStencilOp_Keep;
	depth_state.stencilFrontPassOp = DkStencilOp_Replace;
	depth_state.stencilBackCompareOp = DkCompareOp_Always;
	depth_state.stencilBackFailOp = DkStencilOp_Keep;
	depth_state.stencilBackDepthFailOp = DkStencilOp_Keep;
	depth_state.stencilBackPassOp = DkStencilOp_Replace;
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);
	dkCmdBufSetStencil(m_cmdbuf, DkFace_FrontAndBack, 1, 1, 1);

	const DkShader* shaders[] = {&m_convert_vsh, &m_date_fsh};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	ReserveImageDescriptors(1);
	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	const DkResHandle rt_handle = dkMakeTextureHandle(PushImage(rt), SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 0, &rt_handle, 1);

	const struct
	{
		u32 variant;
		u32 pad[3];
	} ub = {static_cast<u32>(datm), {0, 0, 0}};
	const DkGpuAddr cb_addr = StreamUniform(&ub, sizeof(ub));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 0, cb_addr, AlignUp(sizeof(ub), DK_UNIFORM_BUF_ALIGNMENT));

	// Fullscreen quad over the modified region.
	const GSVector4 dRect(bbox);
	const float left = dRect.x * 2.0f / dssize.x - 1.0f;
	const float top = 1.0f - dRect.y * 2.0f / dssize.y;
	const float right = dRect.z * 2.0f / dssize.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2.0f / dssize.y;
	const ConvertVertex vertices[4] = {
		{{left, top, 0.5f, 1.0f}, {0.0f, 0.0f}},
		{{right, top, 0.5f, 1.0f}, {1.0f, 0.0f}},
		{{left, bottom, 0.5f, 1.0f}, {0.0f, 1.0f}},
		{{right, bottom, 0.5f, 1.0f}, {1.0f, 1.0f}},
	};

	const DkGpuAddr vertex_addr = StreamVertices(vertices, sizeof(vertices));

	static const DkVtxAttribState attribs[2] = {
		{0, 0, offsetof(ConvertVertex, pos), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
		{0, 0, offsetof(ConvertVertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
	};
	static const DkVtxBufferState buffer_state = {sizeof(ConvertVertex), 0};
	dkCmdBufBindVtxAttribState(m_cmdbuf, attribs, 2);
	dkCmdBufBindVtxBufferState(m_cmdbuf, &buffer_state, 1);
	dkCmdBufBindVtxBuffer(m_cmdbuf, 0, vertex_addr, sizeof(vertices));
	WaitForCopyEngine();
	dkCmdBufDraw(m_cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);

	// Order the stencil writes before the main draw's test.
	IssueBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
	g_perfmon.Put(GSPerfMon::Barriers, 1);
}

GSTextureDK* GSDeviceDK::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	if (!m_date_shaders_ok || !m_tfx_shaders_ok)
		return nullptr;

	GSTextureDK* const rt = static_cast<GSTextureDK*>(config.rt);
	GSTextureDK* const ds = static_cast<GSTextureDK*>(config.ds);
	const bool has_ds = (ds && ds->IsDepth());
	const GSVector2i rtsize = rt->GetSize();

	// The per-pixel "lowest failing primitive id" image.
	GSTextureDK* const image =
		static_cast<GSTextureDK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
	{
		Console.Warning("DK3D: failed to allocate PrimID DATE image. Aborting draw.");
		return nullptr;
	}

	const GSVector4 drawareaf(config.drawarea);
	const GSVector4 sRect = drawareaf / GSVector4(rtsize).xyxy();

	// INT_MAX everywhere, -1 where the existing RT alpha already fails the date test.
	{
		const struct
		{
			u32 variant;
			u32 pad[3];
		} ub = {static_cast<u32>(config.datm), {0, 0, 0}};
		DoStretchRectImpl(rt, sRect, image, drawareaf, &m_primid_init_fsh, false, &ub, sizeof(ub));
	}

	// MIN-blend each primitive's id into the image
	BeginFrameIfNeeded();
	WaitForCopyEngine();
	InvalidateHWStateCache();
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	DkImageView image_view;
	image->GetImageView(&image_view);
	DkImageView ds_view;
	if (has_ds)
		ds->GetImageView(&ds_view);
	dkCmdBufBindRenderTarget(m_cmdbuf, &image_view, has_ds ? &ds_view : nullptr);

	const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(rtsize.x), static_cast<float>(rtsize.y), 0.0f, 1.0f};
	dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
	// Scissor to the modified area
	const GSVector4i pp_scissor = config.drawarea.rintersect(GSVector4i::loadh(rtsize));
	const DkScissor dk_scissor = {static_cast<u32>(std::max(0, pp_scissor.x)),
		static_cast<u32>(std::max(0, pp_scissor.y)), static_cast<u32>(std::max(0, pp_scissor.width())),
		static_cast<u32>(std::max(0, pp_scissor.height()))};
	dkCmdBufSetScissors(m_cmdbuf, 0, &dk_scissor, 1);
	image->SetState(GSTexture::State::Dirty);

	DkRasterizerState rasterizer_state;
	dkRasterizerStateDefaults(&rasterizer_state);
	rasterizer_state.cullMode = DkFace_None;
	dkCmdBufBindRasterizerState(m_cmdbuf, &rasterizer_state);

	DkColorState color_state;
	dkColorStateDefaults(&color_state);
	dkColorStateSetBlendEnable(&color_state, 0, true);
	dkCmdBufBindColorState(m_cmdbuf, &color_state);
	DkBlendState blend_state;
	dkBlendStateDefaults(&blend_state);
	dkBlendStateSetOps(&blend_state, DkBlendOp_Min, DkBlendOp_Min);
	dkBlendStateSetFactors(&blend_state, DkBlendFactor_One, DkBlendFactor_Zero, DkBlendFactor_One,
		DkBlendFactor_Zero);
	dkCmdBufBindBlendStates(m_cmdbuf, 0, &blend_state, 1);

	DkColorWriteState color_write_state;
	dkColorWriteStateDefaults(&color_write_state);
	dkColorWriteStateSetMask(&color_write_state, 0, DkColorMask_R);
	dkCmdBufBindColorWriteState(m_cmdbuf, &color_write_state);

	DkDepthStencilState depth_state;
	dkDepthStencilStateDefaults(&depth_state);
	if (has_ds)
	{
		static const DkCompareOp ztst[] = {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Gequal,
			DkCompareOp_Greater};
		depth_state.depthTestEnable = (config.depth.ztst != ZTST_ALWAYS);
		depth_state.depthWriteEnable = false;
		depth_state.depthCompareOp = ztst[config.depth.ztst];
	}
	else
	{
		depth_state.depthTestEnable = false;
		depth_state.depthWriteEnable = false;
	}
	dkCmdBufBindDepthStencilState(m_cmdbuf, &depth_state);

	// The PrimID prepass needs the ubershader sadly
	const DkShader* shaders[] = {&m_tfx_vsh[static_cast<u32>(config.vs.expand)], &m_tfx_fsh[TfxVariantUber]};
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);

	ReserveImageDescriptors(2);
	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	// Bind config.tex/pal so the prepass computes the same alpha the main draw would.
	GSTextureDK* const tex = static_cast<GSTextureDK*>(config.tex);
	GSTextureDK* const pal = static_cast<GSTextureDK*>(config.pal);
	if (tex)
		CommitClear(tex);
	const u32 sampler_slot = (config.sampler.biln ? SAMPLER_LINEAR : SAMPLER_POINT) |
							 (config.sampler.tau ? 2u : 0u) | (config.sampler.tav ? 1u : 0u);
	DkResHandle handles[2];
	handles[0] = tex ? dkMakeTextureHandle(PushImage(tex), sampler_slot) : dkMakeTextureHandle(0, SAMPLER_POINT);
	handles[1] = pal ? dkMakeTextureHandle(PushImage(pal), SAMPLER_POINT) : dkMakeTextureHandle(0, SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Fragment, 2, handles, 2);

	// Select which path to use in the TFX shader
	const DkGpuAddr vs_addr = StreamUniform(&config.cb_vs, sizeof(config.cb_vs));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 1, vs_addr,
		AlignUp(sizeof(config.cb_vs), DK_UNIFORM_BUF_ALIGNMENT));
	const DkGpuAddr ps_addr = StreamUniform(&config.cb_ps, sizeof(config.cb_ps));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 1, ps_addr,
		AlignUp(sizeof(config.cb_ps), DK_UNIFORM_BUF_ALIGNMENT));
	const DKTfxSelector pre_sel = MakeTfxSelector(config.ps, tex != nullptr);
	const DkGpuAddr sel_addr = StreamUniform(&pre_sel, sizeof(pre_sel));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Vertex, 0, sel_addr, AlignUp(sizeof(pre_sel), DK_UNIFORM_BUF_ALIGNMENT));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Fragment, 0, sel_addr, AlignUp(sizeof(pre_sel), DK_UNIFORM_BUF_ALIGNMENT));

	BindHWGeometry(config);
	BindTfxVertexInput(m_cmdbuf, config.vs.expand);
	WaitForCopyEngine();
	dkCmdBufDrawIndexed(m_cmdbuf, GetTfxPrimitive(config.topology), config.nindices, 1, 0, 0, 0);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);

	// Order the prepass writes before the main draw samples the image.
	IssueBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
	g_perfmon.Put(GSPerfMon::Barriers, 1);

	// The main draw (and its alpha second pass) run as DATE 3.
	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;
	return image;
}

void GSDeviceDK::SendHWDraw(const GSHWDrawConfig& config, DkPrimitive primitive, bool one_barrier, bool full_barrier)
{
	if (full_barrier)
	{
		// drawlist groups non-overlapping sprite runs
		if (config.drawlist)
		{
			const u32 indices_per_prim = config.indices_per_prim;
			const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
			for (u32 n = 0, p = 0; n < draw_list_size; n++)
			{
				const u32 count = static_cast<u32>((*config.drawlist)[n]) * indices_per_prim;
				IssueBarrier(DkBarrier_Tiles, DkInvalidateFlags_Image);
				WaitForCopyEngine();
				dkCmdBufDrawIndexed(m_cmdbuf, primitive, count, 1, p, 0, 0);
				p += count;
			}
			g_perfmon.Put(GSPerfMon::Barriers, draw_list_size);
			g_perfmon.Put(GSPerfMon::DrawCalls, draw_list_size);
			return;
		}

		// Overlapping geometry needs a barrier between primitives.
		const u32 indices_per_prim = config.indices_per_prim;
		u32 prims = 0;
		for (u32 p = 0; p < config.nindices; p += indices_per_prim)
		{
			IssueBarrier(DkBarrier_Tiles, DkInvalidateFlags_Image);
			WaitForCopyEngine();
			dkCmdBufDrawIndexed(m_cmdbuf, primitive, indices_per_prim, 1, p, 0, 0);
			prims++;
		}
		g_perfmon.Put(GSPerfMon::Barriers, prims);
		g_perfmon.Put(GSPerfMon::DrawCalls, prims);
		return;
	}

	// A single barrier before the whole draw suffices.
	if (one_barrier)
	{
		IssueBarrier(DkBarrier_Tiles, DkInvalidateFlags_Image);
		g_perfmon.Put(GSPerfMon::Barriers, 1);
	}

	WaitForCopyEngine();
	dkCmdBufDrawIndexed(m_cmdbuf, primitive, config.nindices, 1, 0, 0, 0);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
}
#endif

void GSDeviceDK::ClearSamplerCache()
{
}

GSTexture* GSDeviceDK::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
#ifdef __SWITCH__
	if (m_device)
	{
		std::unique_ptr<GSTextureDK> tex = GSTextureDK::Create(m_device, this, usage, format, width, height, levels);
		if (tex)
			return tex.release();
		return nullptr;
	}
#endif
	return nullptr;
}

void GSDeviceDK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const MergeTopBand* top_band, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
#ifdef __SWITCH__
	if (!dTex)
		return;

	const bool linear = (filter == Filter::Biln);

	GSTextureDK* const dst = static_cast<GSTextureDK*>(dTex);
	GSTextureDK* const src0 = static_cast<GSTextureDK*>(sTex[0]); // alpha-blended foreground
	GSTextureDK* const src1 = static_cast<GSTextureDK*>(sTex[1]); // blend background
	GSTextureDK* const src2 = static_cast<GSTextureDK*>(sTex[2]); // EXTBUF feedback target
	BeginFrameIfNeeded();
	WaitForCopyEngine();

	// A YUV-converted copy of the merge circuit is written back into sTex[2] so it becomes a texture for a later draw
	const bool feedback_write_2 = PMODE.EN2 && src2 != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && src2 != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const bool can_yuv = m_postprocess_shaders_ok && (feedback_write_1 || feedback_write_2);

	struct
	{
		s32 EMODA;
		s32 EMODC;
		u32 pad[2];
	} yuv_cb = {static_cast<s32>(EXTBUF.EMODA), static_cast<s32>(EXTBUF.EMODC), {}};
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);

	const GSVector4 bg_color = GSVector4::unorm8(c);
	float bg[4];
	GSVector4::store<false>(bg, bg_color);

	// Fill the whole target with the PCRTC background colour
	const auto clear_dst_bg = [&]() {
		DkImageView dst_view;
		dst->GetImageView(&dst_view);
		dkCmdBufBindRenderTarget(m_cmdbuf, &dst_view, nullptr);
		m_hw_targets_valid = false;
		const DkViewport viewport = {0.0f, 0.0f, static_cast<float>(dst->GetWidth()),
			static_cast<float>(dst->GetHeight()), 0.0f, 1.0f};
		const DkScissor scissor = {0, 0, static_cast<u32>(dst->GetWidth()), static_cast<u32>(dst->GetHeight())};
		dkCmdBufSetViewports(m_cmdbuf, 0, &viewport, 1);
		dkCmdBufSetScissors(m_cmdbuf, 0, &scissor, 1);
		dkCmdBufClearColorFloat(m_cmdbuf, 0, DkColorMask_RGBA, bg[0], bg[1], bg[2], bg[3]);
		dst->SetState(GSTexture::State::Dirty);
		dst->SetWriteGen(m_gpu_write_gen);
	};

	clear_dst_bg();

	// With feedback_write_2_but_blend_bg it is composited only so the feedback can capture it.
	// It is then overwritten by the background again below.
	const bool composite_output2 = src1 && PMODE.EN2 && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg);
	if (composite_output2)
	{
		DoStretchRectImpl(src1, sRect[1], dst, dRect[1], &m_copy_fsh, linear);
		if (top_band[1].enabled)
			DoStretchRectImpl(src1, top_band[1].src, dst, top_band[1].dst, &m_copy_fsh, linear);
	}

	// Capture circuit 2 into the feedback buffer before output 1 is blended.
	if (feedback_write_2 && composite_output2 && can_yuv)
		DoStretchRectImpl(dst, full_r, src2, dRect[2], &m_yuv_fsh, linear, &yuv_cb, sizeof(yuv_cb));

	// Restore the background colour for the final merge
	if (feedback_write_2_but_blend_bg)
		clear_dst_bg();

	// Output 1 is alpha-blended over the background
	if (src0 && PMODE.EN1)
	{
		if (m_postprocess_shaders_ok)
		{
			struct
			{
				float BGColor[4];
				u32 mmod;
				u32 pad[3];
			} ub = {};
			GSVector4::store<false>(ub.BGColor, bg_color);
			ub.mmod = PMODE.MMOD;
			DoStretchRectImpl(src0, sRect[0], dst, dRect[0], &m_merge_fsh, linear, &ub, sizeof(ub), false, 0xf, true);
			if (top_band[0].enabled)
				DoStretchRectImpl(src0, top_band[0].src, dst, top_band[0].dst, &m_merge_fsh, linear, &ub, sizeof(ub), false, 0xf, true);
		}
		else
		{
			DoStretchRectImpl(src0, sRect[0], dst, dRect[0], &m_copy_fsh, linear);
			if (top_band[0].enabled)
				DoStretchRectImpl(src0, top_band[0].src, dst, top_band[0].dst, &m_copy_fsh, linear);
		}
	}

	// Capture circuit 1 into the feedback buffer after blending
	if (feedback_write_1 && can_yuv)
		DoStretchRectImpl(dst, full_r, src2, dRect[2], &m_yuv_fsh, linear, &yuv_cb, sizeof(yuv_cb));
#endif
}

void GSDeviceDK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
#ifdef __SWITCH__
	if (!m_postprocess_shaders_ok || !sTex || !dTex)
		return;

	const bool linear = (filter == Filter::Biln);

	// cb0: vec4 ZrH + vec4 FieldPad + uint mode
	struct
	{
		float ZrH[4];
		float FieldPad[4];
		u32 mode;
		u32 pad[3];
	} ub = {};
	GSVector4::store<false>(ub.ZrH, cb.ZrH);
	GSVector4::store<false>(ub.FieldPad, cb.FieldPad);
	ub.mode = static_cast<u32>(shader);

	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), sRect, static_cast<GSTextureDK*>(dTex), dRect,
		&m_interlace_fsh, linear, &ub, sizeof(ub));
#endif
}

void GSDeviceDK::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
#ifdef __SWITCH__
	if (!m_postprocess_shaders_ok || !sTex || !dTex)
		return;

	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4(0.0f, 0.0f, 1.0f, 1.0f),
		static_cast<GSTextureDK*>(dTex), GSVector4(dTex->GetRect()), &m_fxaa_fsh, true);
#endif
}

void GSDeviceDK::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
#ifdef __SWITCH__
	if (!m_postprocess_shaders_ok || !sTex || !dTex)
		return;

	// cb0: vec4 params
	const float ub[4] = {params[0], params[1], params[2], params[3]};
	DoStretchRectImpl(static_cast<GSTextureDK*>(sTex), GSVector4(0.0f, 0.0f, 1.0f, 1.0f),
		static_cast<GSTextureDK*>(dTex), GSVector4(dTex->GetRect()), &m_shadeboost_fsh, false, ub, sizeof(ub));
#endif
}

bool GSDeviceDK::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	if (!m_cas_shader_ok || !sTex || !dTex)
		return false;

	GSTextureDK* const sTexDK = static_cast<GSTextureDK*>(sTex);
	GSTextureDK* const dTexDK = static_cast<GSTextureDK*>(dTex);

	BeginFrameIfNeeded();
	WaitForCopyEngine();
	InvalidateHWStateCache();

	// Make the source's prior writes visible
	CommitClear(sTexDK);
	IssueBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
	g_perfmon.Put(GSPerfMon::Barriers, 1);

	ReserveImageDescriptors(2);
	dkCmdBufBindImageDescriptorSet(m_cmdbuf, m_image_descriptor_set, NUM_IMAGE_DESCRIPTORS);
	dkCmdBufBindSamplerDescriptorSet(m_cmdbuf, m_sampler_descriptor_set, NUM_SAMPLERS);

	// Source bound as a sampled texture
	const u32 src_slot = PushImage(sTexDK);
	const DkResHandle src_handle = dkMakeTextureHandle(src_slot, SAMPLER_POINT);
	dkCmdBufBindTextures(m_cmdbuf, DkStage_Compute, 0, &src_handle, 1);

	// Destination bound as a load/store image
	DkImageView dst_view;
	dTexDK->GetImageView(&dst_view);
	DkImageDescriptor dst_descriptor;
	dkImageDescriptorInitialize(&dst_descriptor, &dst_view, true, false);
	const u32 dst_slot = PushImageDescriptor(dst_descriptor);
	const DkResHandle dst_handle = dkMakeImageHandle(dst_slot);
	dkCmdBufBindImages(m_cmdbuf, DkStage_Compute, 0, &dst_handle, 1);

	// CAS constants
	std::array<u32, NUM_CAS_CONSTANTS> ub = constants;
	ub[10] = sharpen_only ? 1u : 0u;
	ub[11] = 0;
	const DkGpuAddr cb_addr = StreamUniform(ub.data(), sizeof(ub));
	dkCmdBufBindUniformBuffer(m_cmdbuf, DkStage_Compute, 0, cb_addr, AlignUp(sizeof(ub), DK_UNIFORM_BUF_ALIGNMENT));

	const DkShader* cas_shader = &m_cas_csh;
	dkCmdBufBindShaders(m_cmdbuf, DkStageFlag_Compute, &cas_shader, 1);

	static constexpr u32 threadGroupWorkRegionDim = 16;
	const u32 dispatchX = (static_cast<u32>(dTexDK->GetWidth()) + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const u32 dispatchY = (static_cast<u32>(dTexDK->GetHeight()) + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	WaitForCopyEngine();
	dkCmdBufDispatchCompute(m_cmdbuf, dispatchX, dispatchY, 1);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);

	// Stamp before the flushing barrier below
	dTexDK->SetWriteGen(m_gpu_write_gen);

	// Write to the present sample
	IssueBarrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
	g_perfmon.Put(GSPerfMon::Barriers, 1);

	dTexDK->SetState(GSTexture::State::Dirty);
	return true;
}
