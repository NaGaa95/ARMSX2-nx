add_library(glad
	include/glad/gl.h
	include/KHR/khrplatform.h
	src/gl.c
)

target_include_directories(glad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(HORIZON)
	target_link_libraries(glad PRIVATE Threads::Threads)
	target_compile_definitions(glad PUBLIC EGL_NO_PLATFORM_SPECIFIC_TYPES)
else()
	target_link_libraries(glad PRIVATE Threads::Threads "${CMAKE_DL_LIBS}")
endif()

if(WIN32)
	target_sources(glad PRIVATE
		include/glad/wgl.h
		src/wgl.c
	)
else()
	target_sources(glad PRIVATE
		include/EGL/eglplatform.h
		include/glad/egl.h
		src/egl.c
	)
endif()
