set(FFMPEG_DIR "/usr/local/ffmpeg")

add_executable(tutorial01 tutorial01.c)

target_include_directories(tutorial01 PRIVATE ${FFMPEG_DIR}/include)
target_link_directories(tutorial01 PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(tutorial01 PRIVATE -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample  -lavdevice  -lz -llzma -lbz2 -lva -lva-drm -lva-x11 -lrt -lm -lX11 -lvdpau)