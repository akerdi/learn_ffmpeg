set(FFMPEG_DIR "/usr/local/ffmpeg")

aux_source_directory(. SRC_LIST)
add_executable(program ${SRC_LIST})

target_include_directories(program PRIVATE ${FFMPEG_DIR}/include)
target_link_directories(program PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(program PRIVATE -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample  -lavdevice  -lz -llzma -lbz2 -lva -lva-drm -lva-x11 -lrt -lm -lX11 -lvdpau)