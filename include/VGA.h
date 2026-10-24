#ifndef __VGA_H__
#define __VGA_H__

#include <cstdint>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

// =============================================================================
// VGA 640x480 @ 60Hz TIMING SPECIFICATIONS
// Pixel Clock Target : 25.2 MHz
// Horizontal Frequency: 31.468 kHz
// Vertical Frequency  : 59.94 Hz
// =============================================================================

#define VGA_WIDTH            640
#define VGA_HEIGHT           480
#define VGA_BYTES_PER_LINE   (VGA_WIDTH / 8)
#define VGA_VRAM_SIZE        (VGA_BYTES_PER_LINE * VGA_HEIGHT)

// Default Pin Assignments (matching Raspberry Pi Pico GPIOs)
#define VGA_PIN_HSYNC        16  // HSYNC Output (Active LOW)
#define VGA_PIN_VSYNC        17  // VSYNC Output (Active LOW)
#define VGA_PIN_VIDEO        18  // Monochrome Video Out (1-bit through ~270 Ohm resistor)

class VGA_class {
public:
    VGA_class();
    void begin(uint8_t* extram = nullptr);
    void end();

    uint8_t* VRAM();
    void cls();

    void delay_frame(uint16_t frames);
    void waitVBlank();
    void waitActive();
    void setBktmStartHook(void (*func)());
    void setBktmEndHook(void (*func)());

    uint16_t width()     { return VGA_WIDTH; }
    uint16_t height()    { return VGA_HEIGHT; }
    uint16_t vram_size() { return VGA_VRAM_SIZE; }
    int dmaChannel() const { return dma_chan; }

    // Scanline driver callback run from alarm/interrupt
    void scanline_handler();

private:
    uint8_t* vram_front;
    uint8_t* vram_storage;

    PIO pio_instance;
    uint sm_video;
    uint pio_offset;
    uint sm_sync;
    uint sync_offset;
    int dma_chan;

    volatile uint16_t current_line;
    void (*blanking_start_hook)();
    void (*blanking_end_hook)();

    void initGPIO();
    void initPIO();
    void initDMA();
};

extern VGA_class VGA;

#endif // __VGA_H__