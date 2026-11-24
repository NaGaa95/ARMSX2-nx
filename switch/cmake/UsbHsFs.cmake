# libusbhsfs mounts USB mass-storage partitions as devoptab paths (ums0:/, ums1:/, ...). The same build as the user's
# other Switch ports (Cemu-nx, Dolphin-nx): ITotalJustice's libusbhsfs at a pinned revision with the UASP transport
# and FatFs lock-order patches in switch/libusbhsfs, FAT/exFAT and NTFS (switch-ntfs-3g), no EXT2/3/4.
# Switch only: on the Switch 2, kaiHEN's usb:hs never answers.

include(FetchContent)

# Enable NTFS independently: USBHSFS_GPL would also pull in EXT2/3/4.
set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS ON CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)

find_library(USBHSFS_NTFS_3G ntfs-3g)
find_path(USBHSFS_NTFS_3G_INCLUDE_DIR ntfs-3g/volume.h)
if(NOT USBHSFS_NTFS_3G OR NOT USBHSFS_NTFS_3G_INCLUDE_DIR)
	message(FATAL_ERROR
		"USB storage requires switch-ntfs-3g. Install it with 'dkp-pacman -S switch-ntfs-3g' "
		"(or 'pacman -S switch-ntfs-3g' in devkitPro's MSYS2).")
endif()

# The patch set is tied to the exact libusbhsfs tree it was audited against, which also keeps a caller-provided
# FETCHCONTENT_SOURCE_DIR_LIBUSBHSFS from getting the transport changes on another revision.
set(_armsx2_libusbhsfs_pinned_revision 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_Declare(libusbhsfs
	GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
	GIT_TAG ${_armsx2_libusbhsfs_pinned_revision})
FetchContent_MakeAvailable(libusbhsfs)

find_package(Git REQUIRED)
execute_process(
	COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}" rev-parse HEAD
	WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
	RESULT_VARIABLE _armsx2_libusbhsfs_revision_result
	OUTPUT_VARIABLE _armsx2_libusbhsfs_revision
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(NOT _armsx2_libusbhsfs_revision_result EQUAL 0 OR
	NOT "${_armsx2_libusbhsfs_revision}" STREQUAL "${_armsx2_libusbhsfs_pinned_revision}")
	message(FATAL_ERROR "libusbhsfs source is not the pinned ${_armsx2_libusbhsfs_pinned_revision} revision")
endif()

# Apply the transport and FatFs patches once against the pinned source.
set(_armsx2_libusbhsfs_uasp_dir "${CMAKE_SOURCE_DIR}/switch/libusbhsfs")
set(_armsx2_libusbhsfs_patches
	"${_armsx2_libusbhsfs_uasp_dir}/0001-add-uasp-transport-hooks.patch"
	"${_armsx2_libusbhsfs_uasp_dir}/0002-fallback-to-bot-interface.patch"
	"${_armsx2_libusbhsfs_uasp_dir}/0003-cleanup-after-uasp-reset.patch"
	"${_armsx2_libusbhsfs_uasp_dir}/0004-fix-fatfs-lock-order.patch")
foreach(_patch IN LISTS _armsx2_libusbhsfs_patches)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
			apply --ignore-space-change --check "${_patch}"
		WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
		RESULT_VARIABLE _patch_check
		OUTPUT_QUIET ERROR_QUIET)
	if(_patch_check EQUAL 0)
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
				apply --ignore-space-change "${_patch}"
			WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
			RESULT_VARIABLE _patch_result)
		if(NOT _patch_result EQUAL 0)
			message(FATAL_ERROR "Failed to apply ${_patch}")
		endif()
	else()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}"
				apply --ignore-space-change --reverse --check "${_patch}"
			WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}"
			RESULT_VARIABLE _patch_reverse_check
			OUTPUT_QUIET ERROR_QUIET)
		if(NOT _patch_reverse_check EQUAL 0)
			message(FATAL_ERROR "The pinned libusbhsfs source does not match ${_patch}")
		endif()
	endif()
endforeach()

target_sources(libusbhsfs PRIVATE
	"${_armsx2_libusbhsfs_uasp_dir}/usbhsfs_uasp.c"
	"${_armsx2_libusbhsfs_uasp_dir}/usbhsfs_uasp.h")
target_include_directories(libusbhsfs PRIVATE
	"${_armsx2_libusbhsfs_uasp_dir}"
	"${libusbhsfs_SOURCE_DIR}/source")
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)

message(STATUS "libusbhsfs ${_armsx2_libusbhsfs_pinned_revision}: FAT, exFAT and NTFS USB storage")
