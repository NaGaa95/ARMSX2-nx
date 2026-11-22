# SPIRV-Cross f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7 (Apache-2.0, see LICENSE): the core and the GLSL backend only,
# for lsfg-deko3d's SPIR-V to GLSL translation (Switch 2 frame generation). Errors are exceptions, which lsfg-deko3d
# catches.
set(SPIRV_CROSS_REVISION "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7" CACHE INTERNAL "")

add_library(spirv-cross-glsl STATIC
	spirv_cfg.cpp
	spirv_cross.cpp
	spirv_cross_parsed_ir.cpp
	spirv_glsl.cpp
	spirv_parser.cpp
)
target_include_directories(spirv-cross-glsl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(spirv-cross-glsl PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(spirv-cross-glsl PRIVATE -fexceptions)
