set(FFMPEG_DIR "/usr/local/ffmpeg")

aux_source_directory(. SRC_LIST)
add_executable(tutorial02 ${SRC_LIST})

target_include_directories(tutorial02 PRIVATE ${FFMPEG_DIR}/include ${SDL2_INCLUDE_DIRS})
target_link_directories(tutorial02 PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(tutorial02 PRIVATE ${SDL2_LIBRARIES} -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample  -lavdevice  -lz -llzma -lbz2 -lva -lva-drm -lva-x11 -lrt -lm -lX11 -lvdpau)