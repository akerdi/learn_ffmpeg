ffdir := /usr/local/ffmpeg

build01:
	cmake -DCMAKE_PREFIX_PATH=$(ffdir) . -B build
	cd build && make

datapath := /mnt/c/Users/shaohong.jiang/Downloads/ffmpeg-video-player-main/ffmpeg-video-player-main
run01:
	./build/tutorial01/tutorial01 ${datapath}/Iron_Man-Trailer_HD.mp4 20

clean:
	rm -rf build

delete_ppm:
	rm *.ppm