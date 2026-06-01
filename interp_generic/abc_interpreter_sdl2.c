#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <abc_interp.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

static abc_interp_t interp;
static abc_host_t host;

#ifdef _WIN32
static SOCKET wire_sock = INVALID_SOCKET;
#else
static int wire_sock = -1;
#endif
static struct sockaddr_in wire_addr_me;
static struct sockaddr_in wire_addr_other;
static uint8_t wire_rx_buf[256];
static uint8_t wire_rx_len = 0;
static uint8_t wire_rx_ptr = 0;
static uint8_t wire_tx_buf[256];
static uint8_t wire_tx_len = 0;
static uint8_t wire_tx_addr = 0;

enum {
    WIRE_PKT_DATA = 1,
    WIRE_PKT_REQ  = 2,
    WIRE_PKT_RES  = 3,
};

static void* data;
static size_t data_size;
static uint64_t start_ticks;

static uint32_t display[128 * 64];

static SDL_AudioSpec audio_desired;
static SDL_AudioSpec audio_obtained;
static SDL_AudioDeviceID audio_device;

static void SDLCALL audio_callback(void* user, uint8_t* stream, int len)
{
    (void)user;
    int16_t* samples = (int16_t*)stream;
    uint32_t num_samples = (uint32_t)len / 2;
    uint32_t sample_rate = (uint32_t)audio_obtained.freq;
    abc_audio(&interp, &host, samples, num_samples, sample_rate);
}

static uint8_t host_prog(void* user, uint32_t addr)
{
    (void)user;
    if(addr < data_size)
        return ((uint8_t*)data)[addr];
    return 0;
}

static uint32_t host_millis(void* user)
{
    (void)user;
    return (uint32_t)(SDL_GetTicks64() - start_ticks);
}

static uint8_t host_buttons(void* user)
{
    (void)user;
    uint8_t b = 0;
    uint8_t const* k = SDL_GetKeyboardState(NULL);
    if(k[SDL_SCANCODE_UP   ]) b |= ABC_BUTTON_U;
    if(k[SDL_SCANCODE_DOWN ]) b |= ABC_BUTTON_D;
    if(k[SDL_SCANCODE_LEFT ]) b |= ABC_BUTTON_L;
    if(k[SDL_SCANCODE_RIGHT]) b |= ABC_BUTTON_R;
    if(k[SDL_SCANCODE_A    ]) b |= ABC_BUTTON_A;
    if(k[SDL_SCANCODE_Z    ]) b |= ABC_BUTTON_A;
    if(k[SDL_SCANCODE_B    ]) b |= ABC_BUTTON_B;
    if(k[SDL_SCANCODE_S    ]) b |= ABC_BUTTON_B;
    if(k[SDL_SCANCODE_X    ]) b |= ABC_BUTTON_B;
    return b;
}

static uint32_t host_rand_seed(void* user)
{
    (void)user;
    return (uint32_t)time(0);
}

static void host_wire_begin(void* user, uint8_t addr)
{
    (void)user; (void)addr;
}

static uint8_t host_wire_request_from(void* user, uint8_t addr, uint8_t count)
{
    (void)user; (void)addr;
#ifdef _WIN32
    if (wire_sock == INVALID_SOCKET) return 0;
#else
    if (wire_sock < 0) return 0;
#endif
    uint8_t pkt[2] = { WIRE_PKT_REQ, count };
    sendto(wire_sock, (const char*)pkt, 2, 0, (struct sockaddr*)&wire_addr_other, sizeof(wire_addr_other));
    
    // synchronous wait for response (simple for local sim)
    uint64_t start = SDL_GetTicks64();
    while (SDL_GetTicks64() - start < 10)
    {
        uint8_t res[257];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int r = recvfrom(wire_sock, (char*)res, sizeof(res), 0, (struct sockaddr*)&from, &fromlen);
        if (r > 1 && res[0] == WIRE_PKT_RES)
        {
            wire_rx_len = (uint8_t)(r - 1);
            if (wire_rx_len > count) wire_rx_len = count;
            memcpy(wire_rx_buf, res + 1, wire_rx_len);
            wire_rx_ptr = 0;
            return wire_rx_len;
        }
        SDL_Delay(1);
    }
    return 0;
}

static uint8_t host_wire_available(void* user)
{
    (void)user;
    if (wire_rx_ptr < wire_rx_len) return wire_rx_len - wire_rx_ptr;
    return 0;
}

static uint8_t host_wire_read(void* user)
{
    (void)user;
    if (wire_rx_ptr < wire_rx_len) return wire_rx_buf[wire_rx_ptr++];
    return 0;
}

static void host_wire_write(void* user, uint8_t value)
{
    (void)user;
    if (wire_tx_len < 255) wire_tx_buf[wire_tx_len++] = value;
}

static void host_wire_begin_transmission(void* user, uint8_t addr)
{
    (void)user;
    wire_tx_addr = addr;
    wire_tx_len = 0;
}

static uint8_t host_wire_end_transmission(void* user)
{
    (void)user;
#ifdef _WIN32
    if (wire_sock == INVALID_SOCKET) return 4;
#else
    if (wire_sock < 0) return 4;
#endif
    uint8_t pkt[257];
    pkt[0] = WIRE_PKT_DATA;
    memcpy(pkt + 1, wire_tx_buf, wire_tx_len);
    sendto(wire_sock, (const char*)pkt, wire_tx_len + 1, 0, (struct sockaddr*)&wire_addr_other, sizeof(wire_addr_other));
    wire_tx_len = 0;
    return 0;
}

static void network_poll()
{
#ifdef _WIN32
    if (wire_sock == INVALID_SOCKET) return;
#else
    if (wire_sock < 0) return;
#endif
    while (true)
    {
        uint8_t res[257];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int r = recvfrom(wire_sock, (char*)res, sizeof(res), 0, (struct sockaddr*)&from, &fromlen);
        if (r <= 0) break;

        if (res[0] == WIRE_PKT_DATA)
        {
            wire_rx_len = (uint8_t)(r - 1);
            memcpy(wire_rx_buf, res + 1, wire_rx_len);
            wire_rx_ptr = 0;
            interp.wire_on_receive_bytes = wire_rx_len;
            interp.wire_on_receive_pending = 1;
        }
        else if (res[0] == WIRE_PKT_REQ)
        {
            interp.wire_on_request_pending = 1;
        }
    }
}

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "Usage: %s <data.bin> [local_port] [remote_port]\n", argv[0]);
        return 1;
    }

    if (argc >= 4)
    {
        int local_port = atoi(argv[2]);
        int remote_port = atoi(argv[3]);
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wire_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long mode = 1;
        ioctlsocket(wire_sock, FIONBIO, &mode);
#else
        wire_sock = socket(AF_INET, SOCK_DGRAM, 0);
        fcntl(wire_sock, F_SETFL, O_NONBLOCK);
#endif
        wire_addr_me.sin_family = AF_INET;
        wire_addr_me.sin_port = htons(local_port);
        wire_addr_me.sin_addr.s_addr = INADDR_ANY;
        bind(wire_sock, (struct sockaddr*)&wire_addr_me, sizeof(wire_addr_me));

        wire_addr_other.sin_family = AF_INET;
        wire_addr_other.sin_port = htons(remote_port);
        wire_addr_other.sin_addr.s_addr = inet_addr("127.0.0.1");
    }

    {
        FILE* f = fopen(argv[1], "rb");
        if(!f)
        {
            fprintf(stderr, "Unable to open \"%s\"\n", argv[1]);
            return 1;
        }

        fseek(f, 0, SEEK_END);
        data_size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);

        data = malloc(data_size);
        if(!data)
        {
            fprintf(stderr, "Unable to allocate buffer for \"%s\"\n", argv[1]);
            fclose(f);
            return 1;
        }

        size_t r = fread(data, 1, data_size, f);
        if(r != data_size)
        {
            fprintf(stderr, "Unable to read \"%s\"\n", argv[1]);
            free(data);
            fclose(f);
            return 1;
        }

        fclose(f);
    }

    int r = 0;

    memset(&interp, 0, sizeof(interp));
    memset(&host, 0, sizeof(host));

    host.prog = host_prog;
    host.millis = host_millis;
    host.buttons = host_buttons;
    host.rand_seed = host_rand_seed;

    host.wire_begin = host_wire_begin;
    host.wire_request_from = host_wire_request_from;
    host.wire_available = host_wire_available;
    host.wire_read = host_wire_read;
    host.wire_write = host_wire_write;
    host.wire_begin_transmission = host_wire_begin_transmission;
    host.wire_end_transmission = host_wire_end_transmission;

    if(0 != SDL_Init(SDL_INIT_EVERYTHING))
    {
        fprintf(stderr, "Unable to initialize SDL\n");
        r = 1;
        goto sdl_quit;
    }

    memset(&audio_desired, 0, sizeof(audio_desired));
    audio_desired.freq = 44100;
    audio_desired.format = AUDIO_S16;
    audio_desired.channels = 1;
    audio_desired.samples = 1024;
    audio_desired.callback = audio_callback;
    audio_desired.userdata = NULL;
    audio_device = SDL_OpenAudioDevice(
        NULL, 0,
        &audio_desired,
        &audio_obtained,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    SDL_PauseAudioDevice(audio_device, 0);

    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    SDL_Window* window = SDL_CreateWindow(
        "ABC Interpreter",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        6 * 128,
        6 * 64,
        0);
    if(!window)
    {
        fprintf(stderr, "Unable to create window\n");
        r = 1;
        goto sdl_quit;
    }

    SDL_Renderer* renderer;
    renderer = SDL_CreateRenderer(window, -1, 0);
    if(!renderer)
    {
        fprintf(stderr, "Unable to create renderer\n");
        r = 1;
        goto sdl_destroy_window;
    }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 128, 64);
    if(!texture)
    {
        fprintf(stderr, "Unable to create display texture\n");
        r = 1;
        goto sdl_destroy_renderer;
    }

    bool quit = false;
    while(!quit)
    {
        SDL_Event e;
        while(SDL_PollEvent(&e))
        {
            if(e.type == SDL_QUIT)
                quit = true;
        }

        SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
        SDL_RenderClear(renderer);

        network_poll();

        bool idle = false;
        static uint8_t wire_req_csp = 0;
        static bool wire_req_active = false;
        for(unsigned i = 0; !idle && i < 100; ++i)
        {
            SDL_LockAudioDevice(audio_device);
            for(unsigned j = 0; !idle && j < 1000; ++j)
            {
                bool was_req = interp.wire_on_request_pending;
                abc_result_t t = abc_run(&interp, &host);
                if (was_req && !interp.wire_on_request_pending)
                {
                    wire_req_active = true;
                    wire_req_csp = interp.csp;
                }
                if (wire_req_active && interp.csp < wire_req_csp)
                {
                    wire_req_active = false;
                    // request handler finished, send response
                    uint8_t pkt[257];
                    pkt[0] = WIRE_PKT_RES;
                    memcpy(pkt + 1, wire_tx_buf, wire_tx_len);
                    sendto(wire_sock, (const char*)pkt, wire_tx_len + 1, 0, (struct sockaddr*)&wire_addr_other, sizeof(wire_addr_other));
                    wire_tx_len = 0;
                }
#ifdef _MSC_VER
                if(t == ABC_RESULT_ERROR)
                    __debugbreak();
#endif
                if(t == ABC_RESULT_IDLE)
                    idle = true;
            }
            SDL_UnlockAudioDevice(audio_device);
        }

        for(unsigned y = 0; y < 64; ++y)
        {
            for(unsigned x = 0; x < 128; ++x)
            {
                uint32_t t = 0xff000000;
                uint32_t b = interp.display[y * 128 + x];
                b = (b * 0xc0) >> 8;
                b += 0x10;
                t += (b << 0);
                t += (b << 8);
                t += (b << 16);
                display[y * 128 + x] = t;
            }
        }
        SDL_UpdateTexture(texture, NULL, display, 128 * sizeof(uint32_t));

        SDL_RenderCopy(renderer, texture, NULL, NULL);

        SDL_RenderPresent(renderer);
    }

    SDL_DestroyTexture(texture);
sdl_destroy_renderer:
    SDL_DestroyRenderer(renderer);
sdl_destroy_window:
    SDL_DestroyWindow(window);
    SDL_CloseAudioDevice(audio_device);
sdl_quit:
    SDL_Quit();
    free(data);

    return r;
}
