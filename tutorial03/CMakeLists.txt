set(FFMPEG_DIR "/usr/local/ffmpeg")

add_executable(video video.async.c)
add_executable(audio audio.async.c)

include_directories(${FFMPEG_DIR}/include ${SDL2_INCLUDE_DIRS})
target_link_directories(video PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(video PRIVATE ${SDL2_LIBRARIES} -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample  -lavdevice  -lz -llzma -lbz2 -lva -lva-drm -lva-x11 -lrt -lm -lX11 -lvdpau)
target_link_directories(audio PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(audio PRIVATE ${SDL2_LIBRARIES} -lavformat -lavcodec -lswscale -lavutil -lavfilter -lswresample  -lavdevice  -lz -llzma -lbz2 -lva -lva-drm -lva-x11 -lrt -lm -lX11 -lvdpau)
