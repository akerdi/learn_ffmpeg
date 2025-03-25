ffdir := /usr/local/ffmpeg
datapath := /mnt/c/Users/shaohong.jiang/Downloads/ffmpeg-video-player-main/ffmpeg-video-player-main

.PHONY: build

build:
	cmake -DCMAKE_PREFIX_PATH=$(ffdir) . -B build
	cd build && make



01:
	./build/tutorial01/tutorial01 ${datapath}/Iron_Man-Trailer_HD.mp4 20
02:
	./build/tutorial02/tutorial02 ${datapath}/Iron_Man-Trailer_HD.mp4 2000
03:
	./build/tutorial03/tutorial03 ${datapath}/Iron_Man-Trailer_HD.mp4 2000


clean:
	rm -rf build tmp
delete_ppm:
	rm *.ppm