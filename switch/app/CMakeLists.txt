# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-3.0+

enable_language(ASM)

add_executable(yadokari
	main.cpp
	HorizonHost.cpp
	HorizonException.cpp
	HorizonExceptionEntry.S
	HorizonThreadWrap.cpp
)

set_source_files_properties(HorizonException.cpp HorizonExceptionEntry.S
	PROPERTIES COMPILE_OPTIONS "-fno-lto")

# USB mass-storage support. Not on the Switch 2: kaiHEN never answers a usb:hs lookup.
if(NOT NINTENDO_SWITCH2)
	include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/UsbHsFs.cmake")
	target_sources(yadokari PRIVATE HorizonUsbStorage.cpp)
	target_compile_definitions(yadokari PRIVATE YADOKARI_USB_STORAGE=1)
endif()

target_include_directories(yadokari PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

set(YADOKARI_NRO_VERSION "${ARMSX2_VERSION}")
if(NOT YADOKARI_NRO_VERSION)
	set(YADOKARI_NRO_VERSION "1.0.0")
endif()

set(YADOKARI_CORE_LINK_TARGETS PCSX2)
if(TARGET PCSX2_LTO)
	list(APPEND YADOKARI_CORE_LINK_TARGETS PCSX2_LTO)
endif()

target_link_libraries(yadokari PRIVATE
	-Wl,--start-group
	${YADOKARI_CORE_LINK_TARGETS}
	PCSX2_FLAGS
	-Wl,--end-group
)
if(NOT NINTENDO_SWITCH2)
	target_link_libraries(yadokari PRIVATE usbhsfs)
endif()

target_link_options(yadokari PRIVATE -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join)

if(NINTENDO_SWITCH2)
	# switch/nx2/nx2_platform.c: input, the exit cleanup of the process's threads and the usb:hs refusal; atexit:
	# main.cpp runs drako3d's GPU release itself before _Exit.
	target_link_options(yadokari PRIVATE
		-Wl,--wrap=hidInitializeKeyboard
		-Wl,--wrap=hidInitializeMouse
		-Wl,--wrap=hidInitializeTouchScreen
		-Wl,--wrap=__syscall_thread_create
		-Wl,--wrap=smGetServiceWrapper
		-Wl,--wrap=atexit)
	target_link_libraries(yadokari PRIVATE "${DEVKITPRO}/portlibs/switch2/lib/libkaihen.a")
endif()

if(ARMSX2_DK3D)
	target_link_libraries(yadokari PRIVATE ${ARMSX2_DK3D_LIBRARY})
endif()

if(USE_VULKAN)
	if(NINTENDO_SWITCH2)
		# mesa-nx's NVK, merged and localized by switch/deps/prepare_mesa_sdk_switch2.sh.
		target_sources(yadokari PRIVATE nvk_compat.cpp)
		target_link_libraries(yadokari PRIVATE
			"${SWITCH_MESA_SDK_ROOT}/lib/libmesa_local.a"
			"${DEVKITPRO}/portlibs/switch/lib/libexpat.a"
			"${DEVKITPRO}/portlibs/switch/lib/libzstd.a"
			"${DEVKITPRO}/portlibs/switch/lib/libz.a"
			stdc++
			m)
	else()
		find_package(PkgConfig REQUIRED)
		if(USE_OPENGL)
			pkg_check_modules(NXVK REQUIRED nxvk-gl)
		else()
			pkg_check_modules(NXVK REQUIRED nxvk)
		endif()

		# newlib shims required by NXVK.
		target_sources(yadokari PRIVATE nvk_compat.cpp)
		target_include_directories(yadokari PRIVATE ${NXVK_INCLUDE_DIRS})
		target_compile_options(yadokari PRIVATE ${NXVK_CFLAGS_OTHER})

		target_link_libraries(yadokari PRIVATE ${NXVK_LDFLAGS})
	endif()
endif()

set(DK3D_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${DK3D_ROMFS_DIR}")
set(DK3D_DKSH_FILES "")

if(ARMSX2_DK3D)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Uam.cmake")

set(DK3D_SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/pcsx2/GS/Renderers/DK3D/shaders")
set(DK3D_ROMFS_SHADER_DIR "${DK3D_ROMFS_DIR}/shaders")
file(MAKE_DIRECTORY "${DK3D_ROMFS_SHADER_DIR}")

# Compile each glsl to dksh with uam (uam2 on the Switch 2).
function(dk3d_add_shader NAME STAGE)
	add_custom_command(
		OUTPUT "${DK3D_ROMFS_SHADER_DIR}/${NAME}.dksh"
		COMMAND "${UAM_EXE}" -s ${STAGE} -o "${DK3D_ROMFS_SHADER_DIR}/${NAME}.dksh" "${DK3D_SHADER_SRC_DIR}/${NAME}.glsl"
		DEPENDS "${DK3D_SHADER_SRC_DIR}/${NAME}.glsl" ${UAM_DEPENDS}
		COMMENT "uam ${STAGE}: ${NAME}.glsl -> ${NAME}.dksh"
		VERBATIM
	)
	set(DK3D_DKSH_FILES ${DK3D_DKSH_FILES} "${DK3D_ROMFS_SHADER_DIR}/${NAME}.dksh" PARENT_SCOPE)
endfunction()

function(dk3d_add_shader_variant SRC_NAME STAGE OUT_NAME DEFINE)
	set(GEN "${CMAKE_CURRENT_BINARY_DIR}/${OUT_NAME}.gen.glsl")
	add_custom_command(
		OUTPUT "${GEN}"
		COMMAND "${CMAKE_COMMAND}"
			-DIN=${DK3D_SHADER_SRC_DIR}/${SRC_NAME}.glsl
			-DDEF=${DEFINE}
			-DOUT=${GEN}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepend_define.cmake"
		DEPENDS "${DK3D_SHADER_SRC_DIR}/${SRC_NAME}.glsl"
			"${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepend_define.cmake"
		COMMENT "gen ${SRC_NAME} variant ${OUT_NAME} (${DEFINE})"
		VERBATIM
	)
	add_custom_command(
		OUTPUT "${DK3D_ROMFS_SHADER_DIR}/${OUT_NAME}.dksh"
		COMMAND "${UAM_EXE}" -s ${STAGE} -o "${DK3D_ROMFS_SHADER_DIR}/${OUT_NAME}.dksh" "${GEN}"
		DEPENDS "${GEN}" ${UAM_DEPENDS}
		COMMENT "uam ${STAGE}: ${OUT_NAME}.dksh"
		VERBATIM
	)
	set(DK3D_DKSH_FILES ${DK3D_DKSH_FILES} "${DK3D_ROMFS_SHADER_DIR}/${OUT_NAME}.dksh" PARENT_SCOPE)
endfunction()

function(dk3d_add_tfx_variant OUT_NAME DEFINE)
	dk3d_add_shader_variant(tfx_fsh frag ${OUT_NAME} ${DEFINE})
	set(DK3D_DKSH_FILES ${DK3D_DKSH_FILES} PARENT_SCOPE)
endfunction()

dk3d_add_shader(convert_vsh vert)
dk3d_add_shader(texture_fsh frag)
dk3d_add_shader(convert_fsh frag)
dk3d_add_shader(convert_int_fsh frag)
dk3d_add_shader(downsample_fsh frag)
dk3d_add_shader(clut_fsh frag)
dk3d_add_shader(convert_8i_fsh frag)
dk3d_add_shader(date_fsh frag)
dk3d_add_shader(primid_init_fsh frag)
dk3d_add_shader(tfx_vsh vert)
dk3d_add_shader(tfx_fsh frag)
dk3d_add_tfx_variant(tfx_fsh_opaque TFX_VARIANT_OPAQUE)
dk3d_add_tfx_variant(tfx_fsh_palette TFX_VARIANT_PALETTE)
dk3d_add_tfx_variant(tfx_fsh_fast TFX_VARIANT_FAST)
dk3d_add_tfx_variant(tfx_fsh_feedback TFX_VARIANT_FEEDBACK)
dk3d_add_tfx_variant(tfx_fsh_feedback_palette TFX_VARIANT_FEEDBACK_PALETTE)
dk3d_add_tfx_variant(tfx_fsh_feedback_fast TFX_VARIANT_FEEDBACK_FAST)
dk3d_add_shader_variant(tfx_vsh vert tfx_vsh_expand_point TFX_VS_EXPAND_POINT)
dk3d_add_shader_variant(tfx_vsh vert tfx_vsh_expand_line TFX_VS_EXPAND_LINE)
dk3d_add_shader_variant(tfx_vsh vert tfx_vsh_expand_line_aa1 TFX_VS_EXPAND_LINE_AA1)
dk3d_add_shader_variant(tfx_vsh vert tfx_vsh_expand_sprite TFX_VS_EXPAND_SPRITE)
dk3d_add_shader(interlace_fsh frag)
dk3d_add_shader(shadeboost_fsh frag)
dk3d_add_shader(fxaa_fsh frag)
dk3d_add_shader(merge_fsh frag)
dk3d_add_shader(yuv_fsh frag)
dk3d_add_shader(imgui_vsh vert)
dk3d_add_shader(imgui_fsh frag)

# CAS needs the AMD FidelityFX headers inlined.
set(DK3D_FFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/common")
set(DK3D_CAS_GEN "${CMAKE_CURRENT_BINARY_DIR}/cas_csh.gen.glsl")
add_custom_command(
	OUTPUT "${DK3D_CAS_GEN}"
	COMMAND "${CMAKE_COMMAND}"
		-DIN=${DK3D_SHADER_SRC_DIR}/cas_csh.glsl
		-DFFX_A=${DK3D_FFX_DIR}/ffx_a.h
		-DFFX_CAS=${DK3D_FFX_DIR}/ffx_cas.h
		-DOUT=${DK3D_CAS_GEN}
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/inline_shader_includes.cmake"
	DEPENDS "${DK3D_SHADER_SRC_DIR}/cas_csh.glsl" "${DK3D_FFX_DIR}/ffx_a.h" "${DK3D_FFX_DIR}/ffx_cas.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/inline_shader_includes.cmake"
	COMMENT "Inlining ffx includes for cas_csh"
	VERBATIM
)
add_custom_command(
	OUTPUT "${DK3D_ROMFS_SHADER_DIR}/cas_csh.dksh"
	COMMAND "${UAM_EXE}" -s comp -o "${DK3D_ROMFS_SHADER_DIR}/cas_csh.dksh" "${DK3D_CAS_GEN}"
	DEPENDS "${DK3D_CAS_GEN}" ${UAM_DEPENDS}
	COMMENT "uam comp cas_csh.dksh"
	VERBATIM
)
list(APPEND DK3D_DKSH_FILES "${DK3D_ROMFS_SHADER_DIR}/cas_csh.dksh")

add_custom_target(yadokari_shaders DEPENDS ${DK3D_DKSH_FILES})
add_dependencies(yadokari yadokari_shaders)
endif()

# Embed resources into the romfs
set(YADOKARI_RESOURCE_SRC_DIR "${CMAKE_SOURCE_DIR}/bin/resources")
set(DK3D_ROMFS_RESOURCE_DIR "${DK3D_ROMFS_DIR}/resources")
set(YADOKARI_RESOURCE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/yadokari_resources.stamp")
file(GLOB_RECURSE YADOKARI_RESOURCE_FILES CONFIGURE_DEPENDS "${YADOKARI_RESOURCE_SRC_DIR}/*")

set(YADOKARI_PATCHES_ZIP "" CACHE FILEPATH "A patches.zip to pack instead of downloading one")
set(YADOKARI_PATCHES_ZIP_URL
	"https://github.com/isztldav/pcsx2_patches/releases/latest/download/patches.zip"
	CACHE STRING "Where to fetch patches.zip when none is given")
set(YADOKARI_PATCHES_COPY_COMMAND "")
if(YADOKARI_PATCHES_ZIP AND EXISTS "${YADOKARI_PATCHES_ZIP}")
	set(_patches_zip "${YADOKARI_PATCHES_ZIP}")
else()
	set(_patches_zip "${CMAKE_CURRENT_BINARY_DIR}/patches.zip")
	if(NOT EXISTS "${_patches_zip}")
		message(STATUS "Fetching patches.zip from ${YADOKARI_PATCHES_ZIP_URL}")
		file(DOWNLOAD "${YADOKARI_PATCHES_ZIP_URL}" "${_patches_zip}" STATUS _patches_status TIMEOUT 120)
		list(GET _patches_status 0 _patches_result)
		if(NOT _patches_result EQUAL 0)
			list(GET _patches_status 1 _patches_error)
			message(WARNING "patches.zip could not be fetched (${_patches_error}); "
				"the NRO will have no built-in game patches")
			file(REMOVE "${_patches_zip}")
			unset(_patches_zip)
		endif()
	endif()
endif()
if(DEFINED _patches_zip)
	list(APPEND YADOKARI_RESOURCE_FILES "${_patches_zip}")
	set(YADOKARI_PATCHES_COPY_COMMAND
		COMMAND ${CMAKE_COMMAND} -E copy "${_patches_zip}" "${DK3D_ROMFS_RESOURCE_DIR}/patches.zip")
endif()

add_custom_command(
	OUTPUT "${YADOKARI_RESOURCE_STAMP}"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${YADOKARI_RESOURCE_SRC_DIR}" "${DK3D_ROMFS_RESOURCE_DIR}"
	${YADOKARI_PATCHES_COPY_COMMAND}
	COMMAND ${CMAKE_COMMAND} -E touch "${YADOKARI_RESOURCE_STAMP}"
	DEPENDS ${YADOKARI_RESOURCE_FILES}
	COMMENT "Copying resources into ROMFS (romfs:/resources)"
	VERBATIM
)
add_custom_target(yadokari_resources DEPENDS "${YADOKARI_RESOURCE_STAMP}")

# Pack the compiled shaders and resources into the NRO's ROMFS.
dkp_add_asset_target(yadokari_romfs "${DK3D_ROMFS_DIR}")
add_dependencies(yadokari_romfs yadokari_resources)
if(TARGET yadokari_shaders)
	add_dependencies(yadokari_romfs yadokari_shaders)
endif()
set_property(TARGET yadokari_romfs APPEND PROPERTY DKP_ASSET_FILES ${DK3D_DKSH_FILES} "${YADOKARI_RESOURCE_STAMP}")

if(NINTENDO_SWITCH2)
	# One core per GPU driver (drako3d cannot open the GPU after Mesa in the same program): the launcher chainloads
	# the core of the renderer it was asked for.
	set(YADOKARI_NRO_NAME "armsx2_${ARMSX2_SWITCH2_RENDERER}")
	set(YADOKARI_NACP_NAME "ARMSX2-nx")
	set(YADOKARI_NACP_AUTHOR "PalindromicBreadLoaf & naga")
else()
	set(YADOKARI_NRO_NAME "yadokari")
	set(YADOKARI_NACP_NAME "Yadokari")
	set(YADOKARI_NACP_AUTHOR "PalindromicBreadLoaf")
endif()

nx_generate_nacp(OUTPUT ${YADOKARI_NRO_NAME}.nacp
	NAME "${YADOKARI_NACP_NAME}"
	AUTHOR "${YADOKARI_NACP_AUTHOR}"
	VERSION "${YADOKARI_NRO_VERSION}"
)
nx_create_nro(yadokari
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${YADOKARI_NRO_NAME}.nro"
	NACP "${CMAKE_CURRENT_BINARY_DIR}/${YADOKARI_NRO_NAME}.nacp"
	ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
	ROMFS yadokari_romfs
)
