#include <stdio.h>
// #include <tchar.h>
#include <SDL_types.h>
#include <SDL.h>

static uint8_t* audio_chunk;
static uint32_t audio_len;
static uint8_t* audio_pos;
int pcm_buffer_size = 4096;


int quit = 0;

void read_audio_data(void* udata, uint8_t* stream, int len) {
    SDL_memset(stream, 0, len);
    if (audio_len == 0) {
        return;
    }

    len = (len > audio_len ? audio_len : len);
    SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);
    audio_pos += len;
    audio_len -= len;
}

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;

    puts(argv[1]);

    if (SDL_Init(SDL_INIT_AUDIO|SDL_INIT_TIMER)) {
        printf("Could not initialize SDL - %s\n", SDL_GetError());
        return 1;
    }
    SDL_AudioSpec spec;
    spec.freq = 16000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 1;
    spec.silence = 0;
    spec.samples = 1024;
    spec.callback = read_audio_data;
    spec.userdata = NULL;

    if (SDL_OpenAudio(&spec, NULL) < 0) {
        printf("Couldn't open audio: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Event event;
    FILE* fp = fopen(argv[1], "rb");
    uint8_t* pcm_buffer = (char*)malloc(pcm_buffer_size);

    SDL_PauseAudio(0);
    int read_size = 0;
    while (1) {
        if (read_size = fread(pcm_buffer, pcm_buffer_size, 1, fp) == 0) {
            break;
        }
        if (feof(fp) != 0) {
            break;
        }
        audio_len = pcm_buffer_size;
        audio_pos = pcm_buffer;
        while (audio_len > 0) {
            SDL_Delay(1);
        }

        SDL_PollEvent(&event);
        switch (event.type) {
            case SDL_QUIT: {
                printf("SDL_QUIT event received. Quitting.\n");
                SDL_Quit();
                quit = 1;
            } break;

            default: {
                // nothing to do
            } break;
        }
        // exit decoding loop if global quit flag is set
        if (quit) {
            break;
        }
    }
    getc(stdin);
    free(pcm_buffer);
    return 0;
}
