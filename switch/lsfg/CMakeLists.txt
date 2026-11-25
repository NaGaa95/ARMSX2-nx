# LSFG 2x frame generation for the Vulkan and DK3D cores of both consoles (see SwitchLSFG.h). SwitchLSFG.cpp and
# SwitchLSFGVulkan.cpp build with the emulator (pcsx2/CMakeLists.txt); these libraries hold the third-party parts, which
# report errors with exceptions.

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
	# lsfg-deko3d: the DLL's SPIR-V through SPIRV-Cross to GLSL, compiled to DKSH on the console, into a cache; then
	# PPSSPP-nx's generator records the passes into the DK3D renderer's command buffer. The compiler is uam2rt (the
	# Switch 2 Mesa SDK's) for drako3d, and Yadokari's uam built as a library (switch/cmake/UamLibrary.cmake, as in
	# PPSSPP-nx) for deko3d.
	if(NINTENDO_SWITCH2)
		find_package(uam2rt CONFIG REQUIRED PATHS "${SWITCH_MESA_SDK_ROOT}/lib/cmake/uam2rt" NO_DEFAULT_PATH)
		set(LSFG_UAM_LIBRARY uam2rt::uam2rt)
		set(LSFG_UAM_COMMIT "uam2rt-${uam2rt_VERSION}")
		set(_lsfg_uam_bridge deko3d/UamBridgeSwitch2.cpp)
	else()
		include("${CMAKE_SOURCE_DIR}/switch/cmake/UamLibrary.cmake")
		set(LSFG_UAM_LIBRARY uam)
		find_package(Git REQUIRED)
		execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${UAM_ROOT}"
			OUTPUT_VARIABLE LSFG_UAM_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
		# (the bridge is in the uam library, which exports only its functions)
		set(_lsfg_uam_bridge)
	endif()
	add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Cross" "${CMAKE_BINARY_DIR}/3rdparty/SPIRV-Cross" EXCLUDE_FROM_ALL)
	set(LSFG_UAM_BRIDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deko3d")
	add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/lsfg-deko3d" "${CMAKE_BINARY_DIR}/3rdparty/lsfg-deko3d" EXCLUDE_FROM_ALL)

	add_library(SwitchLsfgDeko STATIC
		deko3d/LSFGFrameGenerator.cpp
		deko3d/LSFGShaders.cpp
		${_lsfg_uam_bridge})
	set_target_properties(SwitchLsfgDeko PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_include_directories(SwitchLsfgDeko PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/deko3d")
	target_compile_options(SwitchLsfgDeko PRIVATE -fexceptions)
	target_link_libraries(SwitchLsfgDeko PUBLIC lsfg-deko3d PRIVATE ${LSFG_UAM_LIBRARY})
endif()
