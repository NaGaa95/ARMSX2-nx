// SPDX-License-Identifier: GPL-3.0+
// Adapted from PPSSPP-nx's Common/GPU/Vulkan/SwitchLSFG.cpp.

#include "switch/lsfg/SwitchLSFGVulkan.h"
#include "switch/lsfg/SwitchLSFG.h"
#include "switch/lsfg/lsfg_bridge.h"

#include "common/Console.h"

#include "common/Horizon/Horizon.h"
#include "nx2_platform.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace
{
	bool s_device_feature_enabled = false;
	VkInstance s_instance = VK_NULL_HANDLE;
	VkPhysicalDevice s_physical_device = VK_NULL_HANDLE;
	VkDevice s_device = VK_NULL_HANDLE;
	VkQueue s_queue = VK_NULL_HANDLE;
	uint32_t s_queue_family = VK_QUEUE_FAMILY_IGNORED;
	PFN_vkGetInstanceProcAddr s_get_instance_proc_addr = nullptr;

	VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
	VkExtent2D s_extent{};
	std::vector<VkImage> s_swapchain_images;
	bool s_swapchain_compatible = false;
	LsfgNxRuntime* s_runtime = nullptr;
	bool s_init_attempted = false;
	// Whether the last present went through the bridge: after passthrough its history is stale.
	bool s_generated_last = false;
	uint32_t s_observed_speed_control_state = 0;

	uint64_t s_last_source_present_ns = 0;
	double s_source_interval_ns = 0.0;
	unsigned s_source_samples = 0;
	unsigned s_high_rate_slow_samples = 0;
	bool s_previous_requested = false;
	int s_rate_decision = -1;
	// Docked with the 120 Hz option: kept from the first 50/60 FPS measurement until frame generation stops.
	bool s_high_refresh_requested = false;

	void DestroyRuntime()
	{
		if (!s_runtime)
			return;
		lsfg_nx_destroy(s_runtime);
		s_runtime = nullptr;
	}

	void ResetTiming()
	{
		s_last_source_present_ns = 0;
		s_source_interval_ns = 0.0;
		s_source_samples = 0;
		s_high_rate_slow_samples = 0;
		s_previous_requested = false;
		s_rate_decision = -1;
	}

	void ResetSwapchain()
	{
		DestroyRuntime();
		s_swapchain = VK_NULL_HANDLE;
		s_extent = {};
		s_swapchain_images.clear();
		s_swapchain_compatible = false;
		s_init_attempted = false;
		ResetTiming();
	}

	uint64_t MonotonicNs()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	uint64_t ObserveSourcePresent()
	{
		const uint64_t now = MonotonicNs();
		uint64_t interval = 0;
		if (s_last_source_present_ns && now > s_last_source_present_ns)
		{
			interval = now - s_last_source_present_ns;
			if (interval >= 4'000'000 && interval <= 100'000'000)
			{
				s_source_interval_ns = s_source_samples == 0 ? static_cast<double>(interval) :
					s_source_interval_ns * 0.875 + static_cast<double>(interval) * 0.125;
				if (s_source_samples < 120)
					++s_source_samples;
			}
			else
			{
				interval = 0;
			}
		}
		s_last_source_present_ns = now;
		return interval;
	}

	bool SourceIsHighRate()
	{
		return s_source_samples >= 8 && s_source_interval_ns > 0.0 && s_source_interval_ns < 30'000'000.0;
	}

	bool TryCreateRuntime(VkQueue queue, float flow_scale, bool performance)
	{
		if (s_runtime)
			return true;
		if (s_init_attempted || !s_swapchain_compatible || queue != s_queue || !s_instance || !s_physical_device ||
			!s_device || !s_get_instance_proc_addr || s_swapchain_images.size() < 3)
			return false;

		s_init_attempted = true;
		const LsfgNxCreateInfo info{
			.instance = s_instance,
			.physical_device = s_physical_device,
			.device = s_device,
			.queue = s_queue,
			.queue_family_index = s_queue_family,
			.get_instance_proc_addr = s_get_instance_proc_addr,
			.swapchain = s_swapchain,
			.extent = s_extent,
			.swapchain_images = s_swapchain_images.data(),
			.swapchain_image_count = static_cast<uint32_t>(s_swapchain_images.size()),
			.shader_dll_path = SWITCH_LSFG_DLL_PATH,
			.pipeline_cache_path = SWITCH_LSFG_VK_PIPELINE_CACHE,
			.flow_scale = flow_scale,
			.performance_mode = performance,
		};
		s_runtime = lsfg_nx_create(&info);
		if (s_runtime)
			INFO_LOG("LSFG: lsfg-vk ready at {}x{}", s_extent.width, s_extent.height);
		else
			ERROR_LOG("LSFG: lsfg-vk could not start (Lossless.dll, or the device)");
		return s_runtime != nullptr;
	}
} // namespace

bool SwitchLSFG_WantsTimelineSemaphores()
{
	return SwitchLSFG_IsPrepared();
}

bool SwitchLSFG_PrepareDeviceFeatures(VkPhysicalDevice physical_device, bool timeline_extension,
	VkPhysicalDeviceTimelineSemaphoreFeatures& feature)
{
	s_device_feature_enabled = false;
	if (!SwitchLSFG_IsPrepared() || !timeline_extension)
		return false;

	VkPhysicalDeviceTimelineSemaphoreFeatures supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported};
	vkGetPhysicalDeviceFeatures2(physical_device, &features);
	if (!supported.timelineSemaphore)
		return false;

	feature = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	feature.timelineSemaphore = VK_TRUE;
	s_device_feature_enabled = true;
	return true;
}

void SwitchLSFG_SetDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
	uint32_t queue_family, PFN_vkGetInstanceProcAddr get_instance_proc_addr)
{
	s_instance = instance;
	s_physical_device = physical_device;
	s_device = device;
	s_queue = queue;
	s_queue_family = queue_family;
	s_get_instance_proc_addr = get_instance_proc_addr;
}

void SwitchLSFG_ResetDevice()
{
	ResetSwapchain();
	SwitchLSFG_SetStatus(false, false);
	s_instance = VK_NULL_HANDLE;
	s_physical_device = VK_NULL_HANDLE;
	s_device = VK_NULL_HANDLE;
	s_queue = VK_NULL_HANDLE;
	s_queue_family = VK_QUEUE_FAMILY_IGNORED;
	s_get_instance_proc_addr = nullptr;
	s_device_feature_enabled = false;
	// The launcher starts at the TV's own resolution.
	s_high_refresh_requested = false;
	if (nx2DisplaySetFrameGeneration(false))
		nx2DisplayReapply();
}

bool SwitchLSFG_AdjustSwapchainCreateInfo(VkSwapchainCreateInfoKHR& create_info, const VkSurfaceCapabilitiesKHR& capabilities)
{
	if (!SwitchLSFG_IsPrepared() || !s_device_feature_enabled)
		return false;

	constexpr VkImageUsageFlags transfer_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((capabilities.supportedUsageFlags & transfer_usage) != transfer_usage)
		return false;

	VkFormatProperties swapchain_properties{};
	VkFormatProperties rgba_properties{};
	vkGetPhysicalDeviceFormatProperties(s_physical_device, create_info.imageFormat, &swapchain_properties);
	vkGetPhysicalDeviceFormatProperties(s_physical_device, VK_FORMAT_R8G8B8A8_UNORM, &rgba_properties);
	constexpr VkFormatFeatureFlags copy_features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if ((swapchain_properties.optimalTilingFeatures & copy_features) != copy_features ||
		(rgba_properties.optimalTilingFeatures & copy_features) != copy_features)
		return false;

	create_info.imageUsage |= transfer_usage;
	// (clamped: the Switch 2's VI surface has exactly 3 images, and asking for more fails vkCreateSwapchainKHR)
	uint32_t desired_images = create_info.minImageCount + 2;
	if (capabilities.maxImageCount)
		desired_images = std::min(desired_images, capabilities.maxImageCount);
	create_info.minImageCount = std::max(create_info.minImageCount, desired_images);
	create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	return true;
}

void SwitchLSFG_AttachSwapchain(VkSwapchainKHR swapchain, VkExtent2D extent, const VkImage* images, uint32_t image_count,
	bool compatible)
{
	ResetSwapchain();
	s_swapchain = swapchain;
	s_extent = extent;
	s_swapchain_compatible = compatible && image_count >= 3;
	if (images && image_count)
		s_swapchain_images.assign(images, images + image_count);
	SwitchLSFG_SetStatus(s_swapchain_compatible, false);
}

void SwitchLSFG_DetachSwapchain(VkSwapchainKHR swapchain)
{
	if (swapchain && swapchain == s_swapchain)
		ResetSwapchain();
}

bool SwitchLSFG_Present(VkQueue queue, const VkPresentInfoKHR& present_info, VkResult& result)
{
	const SwitchLSFGState state = SwitchLSFG_PollState();
	if (state.reset_session)
	{
		DestroyRuntime();
		s_init_attempted = false;
		ResetTiming();
	}

	// Docked with the 120 Hz option, a 50/60 FPS game moves the output to 120 Hz (1440p or 1080p) while frame generation
	// is on; the resize reaches the swapchain through Host::BeginPresentFrame.
	if (!state.enabled || !state.high_refresh)
		s_high_refresh_requested = false;
	else if (state.active && SourceIsHighRate())
		s_high_refresh_requested = true;
	if (nx2DisplaySetFrameGeneration(s_high_refresh_requested))
		nx2DisplayReapply();

	const bool generated_last = std::exchange(s_generated_last, false);
	if (!s_swapchain_compatible || present_info.swapchainCount != 1 || !present_info.pSwapchains ||
		present_info.pSwapchains[0] != s_swapchain)
		return false;

	if (state.speed_control_state != s_observed_speed_control_state)
	{
		ResetTiming();
		if (s_runtime)
			lsfg_nx_reset_history(s_runtime);
		s_observed_speed_control_state = state.speed_control_state;
	}

	const bool requested = state.active;
	uint64_t source_interval = 0;
	if (!requested || s_rate_decision != 0)
		source_interval = ObserveSourcePresent();

	if (requested != s_previous_requested)
	{
		if (requested)
			s_rate_decision = s_source_samples >= 8 ? (SourceIsHighRate() ? 1 : 0) : -1;
		else
			ResetTiming();
		s_high_rate_slow_samples = 0;
		s_previous_requested = requested;
	}

	if (!requested)
	{
		if (!state.enabled)
		{
			DestroyRuntime();
			s_init_attempted = false;
		}
		SwitchLSFG_SetStatus(true, false);
		return false;
	}

	if (s_rate_decision < 0 && s_source_samples >= 8)
		s_rate_decision = SourceIsHighRate() ? 1 : 0;
	// A 50/60 FPS game shows at its own rate at 60 Hz, until it slows down to a rate frame generation doubles.
	if (s_rate_decision == 1 && nx2DisplayRefreshRate() < 120)
	{
		SwitchLSFG_SetStatus(true, true);
		if (source_interval >= 31'500'000)
			++s_high_rate_slow_samples;
		else if (source_interval)
			s_high_rate_slow_samples = 0;
		if (s_high_rate_slow_samples < 16)
			return false;
		s_rate_decision = 0;
		s_high_rate_slow_samples = 0;
	}
	if (s_rate_decision < 0)
		return false;

	SwitchLSFG_SetStatus(true, false);
	const bool fresh_runtime = !s_runtime;
	if (!TryCreateRuntime(queue, state.flow_scale, state.performance))
	{
		SwitchLSFG_SetStatus(false, false);
		return false;
	}
	if (!fresh_runtime && !generated_last)
		lsfg_nx_reset_history(s_runtime);
	if (lsfg_nx_present(s_runtime, queue, &present_info, &result))
	{
		if (result == VK_ERROR_INITIALIZATION_FAILED)
		{
			// The bridge failed inside the present: frame generation goes off, and the swapchain is rebuilt.
			ERROR_LOG("LSFG: frame generation failed, turned off");
			DestroyRuntime();
			SwitchLSFG_SetStatus(false, false);
			result = VK_ERROR_OUT_OF_DATE_KHR;
			return true;
		}
		s_generated_last = true;
		return true;
	}

	SwitchLSFG_SetStatus(false, false);
	return false;
}
