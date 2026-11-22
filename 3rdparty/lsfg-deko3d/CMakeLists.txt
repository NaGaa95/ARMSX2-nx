# lsfg-deko3d (from PPSSPP-nx's ext/lsfg-deko3d): Lossless.dll's shaders to a drako3d shader cache, and the frame
# generation graph and its scheduling. Copyright 2026 PalindromicBreadLoaf, GPL-2.0-or-later (this file).
#
# Needs: spirv-cross-glsl, uam2rt::uam2rt, and LSFG_UAM_BRIDGE_DIR (the directory of UamBridge.h).

# The shader cache is keyed by the translator's and the compiler's versions.
set(LSFG_SPIRV_CROSS_COMMIT "${SPIRV_CROSS_REVISION}")
set(LSFG_UAM_COMMIT "uam2rt-${uam2rt_VERSION}")
configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/lsfg/common/version.hpp" @ONLY)

add_library(lsfg-deko3d STATIC
	src/binding.cpp src/cache_format.cpp src/cache_load.cpp src/cache_store.cpp
	src/compile_uam.cpp src/dksh.cpp src/error.cpp src/image_graph.cpp
	src/layout.cpp src/pe_resources.cpp src/prepare.cpp src/schedule.cpp
	src/sha256.cpp src/shader_set.cpp src/spirv_module.cpp src/translate.cpp)
target_include_directories(lsfg-deko3d
	PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}/include"
	PRIVATE "${LSFG_UAM_BRIDGE_DIR}")
set_target_properties(lsfg-deko3d PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(lsfg-deko3d PRIVATE -fexceptions)
target_link_libraries(lsfg-deko3d PRIVATE spirv-cross-glsl uam2rt::uam2rt)
