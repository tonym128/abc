#pragma once

#ifndef EEPROM_h
#define EEPROM_h
#endif
#include <ArduboyFX.h>

#ifndef ABC_SHADES
#define ABC_SHADES 2
#endif

#ifndef ABC_SHADES_ADJUST_CONTRAST
#define ABC_SHADES_ADJUST_CONTRAST 1
#endif

namespace ards
{

enum error_t : uint8_t
{
    ERR_NONE,
    ERR_SIG, // signature error
    ERR_IDX, // array index out of bounds
    ERR_DIV, // division by zero
    ERR_ASS, // assertion failed
    ERR_DST, // data stack overflow
    ERR_CST, // call stack overflow
    ERR_FRM, // sprite frame outside of set
    ERR_CPY, // sizes of memcpy dst/src differ
    ERR_FNT, // no font set for text operation
    NUM_ERR,
};

constexpr uint8_t MAX_CALLS = 16;

/*

Building this requires the following linker flags:

-Wl,--section-start=.beforedata=0x800100
-Wl,--section-start=.data=0x800511
    
*/
extern __attribute__((section(".beforedata"))) struct vm_t
{
    uint8_t  stack[256];       // 0x100
    union
    {
        uint8_t globals[704]; // 0x200
        struct
        {
            uint8_t globals[256]; // 0x200
            uint8_t buf0[224];    // 0x300
            uint8_t buf1[224];    // 0x3e0
            uint8_t* cmd_ptr;
            uint8_t* batch_ptr;
            uint8_t  current_plane;
            int8_t   batch_px;
            int8_t   batch_py;
            int8_t   batch_dx;
            int8_t   batch_dy;
        } gs;
    };
    uint24_t calls[MAX_CALLS]; // 0x4c0
    uint8_t  sp;               // 0x4f0
    uint24_t pc;               // 0x4f1
    uint8_t  csp;              // 0x4f4
    uint8_t  error;            // 0x4f5
    uint8_t  frame_dur;        // 0x4f6
    uint8_t  frame_start;      // 0x4f7
    uint8_t  needs_render;     // 0x4f8
    uint8_t  text_mode;        // 0x4f9
    uint24_t text_font;        // 0x4fa
    uint32_t wire_on_receive_pc;
    uint32_t wire_on_request_pc;
    uint8_t  wire_on_receive_bytes;
    volatile uint8_t  wire_on_receive_pending;
    volatile uint8_t  wire_on_request_pending;
} vm;

void vm_run();

}
