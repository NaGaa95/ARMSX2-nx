# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-3.0+

# Switch 2: the DK3D shaders are drako3d modules (SM 8.6), compiled by ounce-devkit's uam2 (same options as uam).
if(NINTENDO_SWITCH2)
	find_program(UAM_EXE NAMES uam2 PATHS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin"
		NO_DEFAULT_PATH)
	if(NOT UAM_EXE)
		message(FATAL_ERROR "uam2 not found in ${DEVKITPRO}/tools/bin: install ouncebrew's ounce-devkit")
	endif()
	message(STATUS "DK3D shader compiler (Switch 2): ${UAM_EXE}")
	set(UAM_DEPENDS "")
	return()
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../externals/uam/source/compiler_iface.cpp")
	set(_uam_default_root "${CMAKE_CURRENT_LIST_DIR}/../externals/uam")
else()
	set(_uam_default_root "${CMAKE_SOURCE_DIR}/../uam")
endif()
set(UAM_ROOT "${_uam_default_root}" CACHE PATH
	"Checkout of https://github.com/PalindromicBreadLoaf/uam (c0d76aa or later)")
option(ARMSX2_USE_PACKAGED_UAM "Compile DK3D shaders with devkitPro's uam." OFF)

if(ARMSX2_USE_PACKAGED_UAM)
	find_program(UAM_EXE NAMES uam PATHS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
	if(NOT UAM_EXE)
		message(FATAL_ERROR "uam not found.")
	endif()
	message(WARNING "Using the packaged uam (${UAM_EXE}). Deko3D may exhibit visual bugs.")
	set(UAM_DEPENDS "")
	return()
endif()

if(NOT EXISTS "${UAM_ROOT}/source/compiler_iface.cpp")
	message(FATAL_ERROR
		"uam sources not found at ${UAM_ROOT}.\n"
		"Clone https://github.com/PalindromicBreadLoaf/uam there or set UAM_ROOT. "
		"-DARMSX2_USE_PACKAGED_UAM=ON uses devkitPro's uam instead, which breaks some effects.")
endif()

find_program(UAM_MESON NAMES meson)
find_program(UAM_NINJA NAMES ninja)
find_program(UAM_BISON NAMES bison)
find_program(UAM_FLEX NAMES flex)
foreach(_tool UAM_MESON UAM_NINJA UAM_BISON UAM_FLEX)
	if(NOT ${_tool})
		message(FATAL_ERROR "${_tool} not found. meson, ninja, bison and flex are needed to build the host uam.")
	endif()
endforeach()

include(ExternalProject)
set(_uam_host_dir "${CMAKE_BINARY_DIR}/uam-host")
set(UAM_EXE "${_uam_host_dir}/uam")

# uam's meson.build checks Mako's version with distutils.version.StrictVersion, and Python 3.12 removed distutils:
# that check gets a StrictVersion of its own on PYTHONPATH (a Windows path for MSYS2's native Pythons).
set(_uam_python_shim "${CMAKE_BINARY_DIR}/uam-host-python")
file(WRITE "${_uam_python_shim}/distutils/__init__.py" "")
file(WRITE "${_uam_python_shim}/distutils/version.py" [=[
class StrictVersion:
    def __init__(self, version):
        self.version = tuple(int(part) for part in version.split(".")[:3] if part.isdigit())
    def __gt__(self, other):
        return self.version > other.version
    def __ge__(self, other):
        return self.version >= other.version
    def __lt__(self, other):
        return self.version < other.version
    def __le__(self, other):
        return self.version <= other.version
    def __eq__(self, other):
        return self.version == other.version
]=])
# The host compilers, by absolute path: meson records the ones it finds at configure, and the build step's PATH (MSYS2's
# tools first) would otherwise resolve them to the MSYS runtime's gcc, which cannot build uam.
find_program(UAM_HOST_CC NAMES clang cc gcc PATHS /clangarm64/bin /ucrt64/bin /mingw64/bin /usr/local/bin /usr/bin
	NO_DEFAULT_PATH)
find_program(UAM_HOST_CXX NAMES clang++ c++ g++ PATHS /clangarm64/bin /ucrt64/bin /mingw64/bin /usr/local/bin /usr/bin
	NO_DEFAULT_PATH)
if(NOT UAM_HOST_CC OR NOT UAM_HOST_CXX)
	message(FATAL_ERROR "No host C/C++ compiler for uam: set UAM_HOST_CC and UAM_HOST_CXX")
endif()

set(_uam_python_path "${_uam_python_shim}")
set(_uam_host_cc "${UAM_HOST_CC}")
set(_uam_host_cxx "${UAM_HOST_CXX}")
find_program(_uam_cygpath NAMES cygpath)
if(_uam_cygpath)
	foreach(_var _uam_python_path _uam_host_cc _uam_host_cxx)
		execute_process(COMMAND "${_uam_cygpath}" -m "${${_var}}" OUTPUT_VARIABLE ${_var} OUTPUT_STRIP_TRAILING_WHITESPACE)
	endforeach()
endif()
message(STATUS "uam host compilers: ${_uam_host_cc}, ${_uam_host_cxx}")

ExternalProject_Add(uam_host
	SOURCE_DIR "${UAM_ROOT}"
	BINARY_DIR "${_uam_host_dir}"
	CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${_uam_host_cc}" "CXX=${_uam_host_cxx}"
		"PYTHONPATH=${_uam_python_path}"
		"${UAM_MESON}" setup --buildtype=release "${_uam_host_dir}" "${UAM_ROOT}"
	BUILD_COMMAND "${CMAKE_COMMAND}" -E env --unset=CC --unset=CXX "${UAM_NINJA}" -C "${_uam_host_dir}"
	INSTALL_COMMAND ""
	BUILD_ALWAYS ON
	BUILD_BYPRODUCTS "${UAM_EXE}"
)
set(UAM_DEPENDS uam_host "${UAM_EXE}")
