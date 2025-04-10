ffdir := /usr/local/ffmpeg
datapath := /mnt/c/Users/shaohong.jiang/Downloads/ffmpeg-video-player-main/ffmpeg-video-player-main

opt := $(if $(DEBUG),-DCMAKE_PREFIX_PATH=$(ffdir) -DCMAKE_BUILD_TYPE=Debug,-DCMAKE_PREFIX_PATH=$(ffdir))
build_dir := build

.PHONY: build

build:
	cmake ${opt} . -B ${build_dir}
	cd build && make -j 20

01:
	./${build_dir}/tutorial01/tutorial01 ${datapath}/Iron_Man-Trailer_HD.mp4 20
02:
	./${build_dir}/tutorial02/tutorial02 ${datapath}/Iron_Man-Trailer_HD.mp4 2000
03:
	./${build_dir}/tutorial03/tutorial03 ${datapath}/Iron_Man-Trailer_HD.mp4 2000
03a:
	./${build_dir}/tutorial03/audio ${datapath}/Iron_Man-Trailer_HD.mp4 2000
03v:
	./${build_dir}/tutorial03/video ${datapath}/Iron_Man-Trailer_HD.mp4 2000


clean:
	rm -rf build tmp
delete_ppm:
	rm *.ppm