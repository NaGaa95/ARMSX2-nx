// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

// The Vulkan core's hooks for LSFG 2x (lsfg-vk through lsfg_bridge), adapted from PPSSPP-nx: the device needs timeline
// semaphores, the swapchain transfer usage and FIFO, and each present goes through the bridge, which presents the
// generated frame and then the real one.

// Whether the device should enable VK_KHR_timeline_semaphore (PCSX2's instance is Vulkan 1.1).
bool SwitchLSFG_WantsTimelineSemaphores();
// Fills `feature` (to chain into VkDeviceCreateInfo) when frame generation can use the device.
bool SwitchLSFG_PrepareDeviceFeatures(VkPhysicalDevice physical_device, bool timeline_extension,
	VkPhysicalDeviceTimelineSemaphoreFeatures& feature);
void SwitchLSFG_SetDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, VkQueue queue,
	uint32_t queue_family, PFN_vkGetInstanceProcAddr get_instance_proc_addr);
void SwitchLSFG_ResetDevice();

// Before vkCreateSwapchainKHR: true when the swapchain can carry generated frames.
bool SwitchLSFG_AdjustSwapchainCreateInfo(VkSwapchainCreateInfoKHR& create_info, const VkSurfaceCapabilitiesKHR& capabilities);
void SwitchLSFG_AttachSwapchain(VkSwapchainKHR swapchain, VkExtent2D extent, const VkImage* images, uint32_t image_count,
	bool compatible);
void SwitchLSFG_DetachSwapchain(VkSwapchainKHR swapchain);

// In place of vkQueuePresentKHR: true when it presented (result set), false to present normally.
bool SwitchLSFG_Present(VkQueue queue, const VkPresentInfoKHR& present_info, VkResult& result);
