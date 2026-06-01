#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <abc_interp.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static abc_interp_t interp;
static abc_host_t host;

static void* data;
static size_t data_size;
static uint64_t start_ticks;

static uint32_t display[128 * 64];

static int i2c_sock = -1;
static uint8_t i2c_addr = 0;
static struct sockaddr_in i2c_addr_in;
static uint16_t i2c_my_port = 0;
static uint32_t current_handshake_uuid = 0;

static void host_debug_putc(void* user, char c)
{
    (void)user;
    putchar(c);
    fflush(stdout);
}

static void host_i2c_begin(void* user)
{
    (void)user;
    if(i2c_sock != -1) return;
    i2c_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(i2c_sock == -1) return;

    memset(&i2c_addr_in, 0, sizeof(i2c_addr_in));
    i2c_addr_in.sin_family = AF_INET;
    i2c_addr_in.sin_addr.s_addr = inet_addr("127.0.0.1");

    for(uint16_t port = 12345; port < 12355; ++port)
    {
        i2c_addr_in.sin_port = htons(port);
        if(bind(i2c_sock, (struct sockaddr*)&i2c_addr_in, sizeof(i2c_addr_in)) == 0)
        {
            i2c_my_port = port;
            break;
        }
    }

    if(i2c_my_port == 0)
    {
        fprintf(stderr, "I2C: Failed to bind to any port in range 12345-12354\n");
        close(i2c_sock);
        i2c_sock = -1;
        return;
    }

    printf("I2C: Socket bound to port %d\n", i2c_my_port);
    fcntl(i2c_sock, F_SETFL, O_NONBLOCK);
}

static void i2c_broadcast(uint8_t* buf, size_t len)
{
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr("127.0.0.1");
    for(uint16_t port = 12345; port < 12355; ++port)
    {
        if(port == i2c_my_port) continue; // Don't send to self
        dest.sin_port = htons(port);
        if(sendto(i2c_sock, buf, (socklen_t)len, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0)
        {
            if(errno != ECONNREFUSED) // Normal if port is closed
                perror("I2C: sendto failed");
        }
    }
}

static uint8_t i2c_read_reply_addr = 0;
static uint8_t i2c_read_reply_target = 0;
static uint8_t i2c_read_reply_buf[32];
static uint8_t i2c_read_reply_size = 0;
static bool i2c_read_reply_received = false;

static void process_i2c_packets(abc_interp_t* interp)
{
    if(i2c_sock == -1) return;
    uint8_t buf[64];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    while(recvfrom(i2c_sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &src_len) > 0)
    {
        if(buf[0] == 'W')
        {
            uint8_t target = buf[2];
            if(target == 0 || target == i2c_addr)
            {
                uint8_t size = buf[3];
                if(size > 32) size = 32;
                memcpy(interp->i2c_remote_buf, buf + 4, size);
                interp->i2c_remote_size = size;
            }
        }
        else if(buf[0] == 'R')
        {
            uint8_t target = buf[2];
            if(target == i2c_addr)
            {
                uint8_t reply[40] = { 'A', i2c_addr, buf[1], interp->i2c_local_size };
                memcpy(reply + 4, interp->i2c_local_buf, interp->i2c_local_size);
                i2c_broadcast(reply, interp->i2c_local_size + 4);
            }
        }
        else if(buf[0] == 'A')
        {
            i2c_read_reply_addr = buf[1];
            i2c_read_reply_target = buf[2];
            uint8_t size = buf[3];
            if(size > 32) size = 32;
            memcpy(i2c_read_reply_buf, buf + 4, size);
            i2c_read_reply_size = size;
            i2c_read_reply_received = true;
        }
        else if(buf[0] == 'H')
        {
            // Peer is looking for us, broadcast our presence
            uint8_t h_buf[5];
            h_buf[0] = 'H';
            memcpy(h_buf + 1, &current_handshake_uuid, 4);
            i2c_broadcast(h_buf, 5);
        }
    }
}

static uint8_t host_i2c_handshake(void* user, uint8_t num_players)
{
    (void)user;
    if(i2c_sock == -1) return 0;
    if(num_players < 2) return 0;

    uint32_t my_uuid = (uint32_t)rand();
    current_handshake_uuid = my_uuid;
    printf("Handshake starting... My UUID: %08x, Port: %d\n", my_uuid, i2c_my_port);
    uint32_t uuids[256];
    uuids[0] = my_uuid;
    int num_uuids = 1;

    uint32_t start = SDL_GetTicks();
    uint32_t last_send = 0;
    while(SDL_GetTicks() - start < 3000) // 3 second timeout
    {
        uint32_t now = SDL_GetTicks();
        if(now - last_send >= 200)
        {
            uint8_t h_buf[5];
            h_buf[0] = 'H';
            memcpy(h_buf + 1, &my_uuid, 4);
            i2c_broadcast(h_buf, 5);
            last_send = now;
        }

        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        uint8_t r_buf[64];
        while(recvfrom(i2c_sock, r_buf, sizeof(r_buf), 0, (struct sockaddr*)&src, &src_len) > 0)
        {
            if(r_buf[0] == 'H')
            {
                uint32_t u;
                memcpy(&u, r_buf + 1, 4);
                if(u == my_uuid) continue;
                
                bool found = false;
                for(int i = 0; i < num_uuids; ++i)
                {
                    if(uuids[i] == u) { found = true; break; }
                }
                if(!found && num_uuids < 256)
                {
                    printf("I2C: Discovered peer UUID %08x\n", u);
                    uuids[num_uuids++] = u;
                }
            }
            else
            {
                process_i2c_packets(&interp);
            }
        }

        if(num_uuids >= num_players) 
        {
            // Stay in loop for at least 1s total to make sure others see us
            if(SDL_GetTicks() - start > 1000) break;
        }
        SDL_Delay(20);
    }

    // Sort UUIDs
    for(int i = 0; i < num_uuids - 1; ++i)
    {
        for(int j = i + 1; j < num_uuids; ++j)
        {
            if(uuids[i] > uuids[j])
            {
                uint32_t t = uuids[i];
                uuids[i] = uuids[j];
                uuids[j] = t;
            }
        }
    }

    uint8_t id = 0;
    for(int i = 0; i < num_uuids; ++i)
    {
        if(uuids[i] == my_uuid)
        {
            id = (uint8_t)i;
            break;
        }
    }

    i2c_addr = 8 + id;
    printf("I2C Handshake successful! Player ID: %d, Address: %d\n", id, i2c_addr);
    return id;
}

static void host_i2c_write(void* user, uint8_t addr, uint8_t data)
{
    (void)user;
    if(i2c_sock == -1) return;
    uint8_t buf[5] = { 'W', i2c_addr, addr, 1, data };
    i2c_broadcast(buf, 5);
}

static void host_i2c_write_buf(void* user, uint8_t addr, uint8_t const* data, uint8_t size)
{
    (void)user;
    if(i2c_sock == -1) return;
    uint8_t buf[40] = { 'W', i2c_addr, addr, size };
    if(size > 36) size = 36;
    memcpy(buf + 4, data, size);
    i2c_broadcast(buf, size + 4);
}

static uint8_t host_i2c_read(void* user, uint8_t addr)
{
    (void)user;
    if(i2c_sock == -1) return 0;
    i2c_read_reply_received = false;
    uint8_t buf[3] = { 'R', i2c_addr, addr };
    i2c_broadcast(buf, 3);

    uint32_t start = SDL_GetTicks();
    while(SDL_GetTicks() - start < 200)
    {
        process_i2c_packets(&interp);
        if(i2c_read_reply_received && i2c_read_reply_addr == addr && i2c_read_reply_target == i2c_addr)
        {
            return i2c_read_reply_buf[0];
        }
        SDL_Delay(5);
    }
    return 0;
}

static uint8_t host_i2c_read_buf(void* user, uint8_t addr, uint8_t* data, uint8_t size)
{
    (void)user;
    if(i2c_sock == -1) return 0;
    i2c_read_reply_received = false;
    uint8_t buf[3] = { 'R', i2c_addr, addr };
    i2c_broadcast(buf, 3);

    uint32_t start = SDL_GetTicks();
    while(SDL_GetTicks() - start < 200)
    {
        process_i2c_packets(&interp);
        if(i2c_read_reply_received && i2c_read_reply_addr == addr && i2c_read_reply_target == i2c_addr)
        {
            uint8_t rsize = i2c_read_reply_size;
            if(rsize > size) rsize = size;
            memcpy(data, i2c_read_reply_buf, rsize);
            return rsize;
        }
        SDL_Delay(5);
    }
    return 0;
}


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

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "Usage: %s <data.bin>\n", argv[0]);
        return 1;
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

    srand((unsigned int)time(0) ^ getpid());
    memset(&interp, 0, sizeof(interp));
    memset(&host, 0, sizeof(host));

    host.prog = host_prog;
    host.millis = host_millis;
    host.buttons = host_buttons;
    host.debug_putc = host_debug_putc;
    host.rand_seed = host_rand_seed;
    host.i2c_begin = host_i2c_begin;
    host.i2c_handshake = host_i2c_handshake;
    host.i2c_write = host_i2c_write;
    host.i2c_read = host_i2c_read;
    host.i2c_write_buf = host_i2c_write_buf;
    host.i2c_read_buf = host_i2c_read_buf;

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

        process_i2c_packets(&interp);

        SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
        SDL_RenderClear(renderer);

        bool idle = false;
        for(unsigned i = 0; !idle && i < 100; ++i)
        {
            SDL_LockAudioDevice(audio_device);
            for(unsigned j = 0; !idle && j < 1000; ++j)
            {
                abc_result_t t = abc_run(&interp, &host);
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
