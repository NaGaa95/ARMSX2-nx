# LSFG 2x frame generation for the Switch 2 cores (see SwitchLSFG.h). SwitchLSFG.cpp and SwitchLSFGVulkan.cpp build with
# the emulator (pcsx2/CMakeLists.txt); these libraries hold the third-party parts, which report errors with exceptions.

if(USE_VULKAN)
	# lsfg-vk (from PPSSPP-nx's ext/lsfg-vk) behind lsfg_bridge: it borrows the core's device and queue.
	file(GLOB_RECURSE SWITCH_LSFG_VK_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/3rdparty/lsfg-vk/lsfg-vk-common/src/*.cpp"
		"${CMAKE_SOURCE_DIR}/3rdparty/lsfg-vk/lsfg-vk-backend/src/*.cpp")
	add_library(SwitchLsfgVulkan STATIC lsfg_bridge.cpp ${SWITCH_LSFG_VK_SOURCES})
	set_target_properties(SwitchLsfgVulkan PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_include_directories(SwitchLsfgVulkan PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}"
		"${CMAKE_SOURCE_DIR}/3rdparty/lsfg-vk/lsfg-vk-common/include"
		"${CMAKE_SOURCE_DIR}/3rdparty/lsfg-vk/lsfg-vk-backend/include"
		"${CMAKE_SOURCE_DIR}/3rdparty/lsfg-vk/lsfg-vk-backend/src")
	target_link_libraries(SwitchLsfgVulkan PRIVATE vulkan-headers)
	target_compile_definitions(SwitchLsfgVulkan PRIVATE VK_USE_PLATFORM_VI_NN VK_NO_PROTOTYPES ARMSX2_LSFG_VULKAN_DISPATCH)
	target_compile_options(SwitchLsfgVulkan PRIVATE -fexceptions)
endif()

if(ARMSX2_DK3D)
	# lsfg-deko3d: the DLL's SPIR-V through SPIRV-Cross to GLSL, compiled to DKSH by uam2rt (the Mesa SDK's), on the
	# console, into a cache; then PPSSPP-nx's generator records the passes into the DK3D renderer's command buffer.
	find_package(uam2rt CONFIG REQUIRED PATHS "${SWITCH_MESA_SDK_ROOT}/lib/cmake/uam2rt" NO_DEFAULT_PATH)
	add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Cross" "${CMAKE_BINARY_DIR}/3rdparty/SPIRV-Cross" EXCLUDE_FROM_ALL)
	set(LSFG_UAM_BRIDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deko3d")
	add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/lsfg-deko3d" "${CMAKE_BINARY_DIR}/3rdparty/lsfg-deko3d" EXCLUDE_FROM_ALL)

	add_library(SwitchLsfgDeko STATIC
		deko3d/LSFGFrameGenerator.cpp
		deko3d/LSFGShaders.cpp
		deko3d/UamBridgeSwitch2.cpp)
	set_target_properties(SwitchLsfgDeko PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_include_directories(SwitchLsfgDeko PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/deko3d")
	target_compile_options(SwitchLsfgDeko PRIVATE -fexceptions)
	target_link_libraries(SwitchLsfgDeko PUBLIC lsfg-deko3d PRIVATE uam2rt::uam2rt)
endif()
