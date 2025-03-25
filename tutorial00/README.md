
## FFmpeg

    wget https://ffmpeg.org/releases/ffmpeg-4.4.tar.gz
    tar -zxvf ffmpeg-4.4.tar.gz
    sudo /usr/local/ffmpeg
    cd ffmpeg-4.4
    ./configure --enable-debug=3 --prefix=/usr/local/ffmpeg
    make && make install

```sh
vim .bashrc
# add path to PATH
#export PATH=$PATH:/usr/local/ffmpeg/bin
source .bashrc
```

test:

    ffmpeg

## SDL2

    git clone https://github.com/libsdl-org/SDL.git -b SDL2
    cd SDL
    mkdir build && cd build
    ../configure
    make -j20
    sudo make install

### 解决WSL 无法发声问题

下载windows 软件: http://bosmans.ch/pulseaudio/pulseaudio-1.1.zip

并解压到c:\pulse 中:

    C:\pulse\[etc|bin|lib|share]

接着改动c:\pulse\etc 下两个文件各几处配置:

```diff
@file c:\pulse\etc\pulse\default.pa
@line 42
- ### load-module module-waveout sink_name=output source_name=input
+ load-module module-waveout sink_name=output source_name=input record=0
@line 61
- #load-module module-esound-protocol-tcp
+ load-module module-native-protocol-tcp auth-ip-acl=127.0.0.1 auth-anonymous=1 listen=127.0.0.1

@file c:\pulse\etc\pulse\daemon.conf
- ; exit-idle-time = 20
+ exit-idle-time = -1 ; 永久启动, 防止空闲时自动退出问题
```

> Windows 中无法自动退出, 需要自己去TaskManager 中执行退出该程序!

在PowerShell(或者cmd), 并`cd c:\pulse\bin`.

    .\pulseaudio.exe --exit-idel-time=-1 -vvvv # -vvvv参数用于增加日志输出的详细程度, 有助于调试. 如果一切正常, 你应该会看到许多日志输出, 并显示Synced 表示pulseAudio 服务端已运行成功.

我们去WSL中验证是否联通:

    sudo apt update
    sudo apt upgrade
    sudo apt install pulseaudio

其他教程会设置环境变量: `export PULSE_SERVER=tcp:127.0.0.1`, 但是我这里报错:`N: [pulseaudio] main.c: User-configured server at tcp:127.0.0.1, refusing to start/autospawn.`. 据此找到[文章](https://bbs.archlinux.org/viewtopic.php?id=186999)说去除这个环境变量: `unset PULSE_SERVER` 然后我这里就执行正常:

    paplay -p /mnt/c/Windows/Media/ding.wav

如果一切正常，你应该会听到Windows系统声音中的"叮"声。