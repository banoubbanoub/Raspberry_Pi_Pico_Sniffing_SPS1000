#include "VGA.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/pio_instructions.h"

VGA_class VGA;

static uint16_t vga_out_instructions[7];
static const pio_program vga_out_program = {
    vga_out_instructions,
    7,
    -1
};
static uint16_t vga_sync_instructions[10];
static const pio_program vga_sync_program = {
    vga_sync_instructions, 10, -1
};

// Dynamic VRAM allocation fallback buffer
static uint8_t internal_vram[VGA_VRAM_SIZE] __attribute__((aligned(4)));

// 640x480 at 60 Hz with a 25.2 MHz pixel clock.
constexpr uint16_t H_ACTIVE = 640;
constexpr uint16_t H_FP     = 16;
constexpr uint16_t H_SYNC   = 96;
constexpr uint16_t H_BP     = 48;
constexpr uint16_t H_TOTAL  = 800; // Total pixel clocks per line

constexpr uint16_t V_ACTIVE = 480;
constexpr uint16_t V_FP     = 10;
constexpr uint16_t V_SYNC   = 2;
constexpr uint16_t V_BP     = 33;
constexpr uint16_t V_TOTAL  = 525; // Total lines per frame

// First DMA word is the pixel count; remaining words contain MSB-first pixels.
static uint32_t ordered_line[1 + VGA_BYTES_PER_LINE / 4];

VGA_class::VGA_class()
    : vram_front(nullptr), vram_storage(internal_vram),
      pio_instance(pio0), sm_video(0), dma_chan(-1), current_line(0),
      blanking_start_hook(nullptr), blanking_end_hook(nullptr) {}

static void __not_in_flash_func(vga_line_irq)() {
    if (pio_interrupt_get(pio0, 1)) {
        pio_interrupt_clear(pio0, 1);
        VGA.scanline_handler();
    }
}

uint8_t* VGA_class::VRAM() {
    return vram_front;
}

void VGA_class::cls() {
    if (vram_front) {
        memset(vram_front, 0, VGA_VRAM_SIZE);
    }
}

void VGA_class::waitVBlank() {
    while (current_line < V_ACTIVE) {
        tight_loop_contents();
    }
}

void VGA_class::waitActive() {
    while (current_line >= V_ACTIVE) {
        tight_loop_contents();
    }
}

void VGA_class::setBktmStartHook(void (*func)()) {
    blanking_start_hook = func;
}

void VGA_class::setBktmEndHook(void (*func)()) {
    blanking_end_hook = func;
}

void VGA_class::delay_frame(uint16_t frames) {
    while (frames--) {
        waitActive();
        waitVBlank();
    }
}

void __not_in_flash_func(VGA_class::scanline_handler)() {
    if (current_line == V_ACTIVE && blanking_start_hook) {
        blanking_start_hook();
    }

    // VSYNC Pulse logic (lines 490 & 491 active LOW)
    if (current_line >= (V_ACTIVE + V_FP) && current_line < (V_ACTIVE + V_FP + V_SYNC)) {
        gpio_put(VGA_PIN_VSYNC, false);
    } else {
        gpio_put(VGA_PIN_VSYNC, true);
    }

    ordered_line[0] = H_ACTIVE - 1;
    if (current_line < V_ACTIVE) {
        const uint8_t* source = vram_front + (current_line * VGA_BYTES_PER_LINE);
        for (uint16_t word = 0; word < VGA_BYTES_PER_LINE / 4; ++word) {
            const uint16_t byte = word * 4;
            ordered_line[word + 1] =
                (static_cast<uint32_t>(source[byte]) << 24) |
                (static_cast<uint32_t>(source[byte + 1]) << 16) |
                (static_cast<uint32_t>(source[byte + 2]) << 8) |
                static_cast<uint32_t>(source[byte + 3]);
        }

    } else {
        memset(ordered_line + 1, 0, VGA_BYTES_PER_LINE);
    }
    dma_channel_transfer_from_buffer_now(
        dma_chan, ordered_line, 1 + VGA_BYTES_PER_LINE / 4);

    // Advance scanline counter
    current_line++;
    if (current_line >= V_TOTAL) {
        current_line = 0;
        if (blanking_end_hook) {
            blanking_end_hook();
        }
    }
}

void VGA_class::begin(uint8_t* extram) {
    if (extram != nullptr) {
        vram_front = extram;
    } else {
        vram_front = vram_storage;
    }
    cls();

    // Set system clock precisely to 126 MHz (5 x 25.2 MHz VGA Pixel Clock)
    set_sys_clock_khz(126000, true);

    initGPIO();
    initPIO();
    initDMA();

    current_line = 0;
    pio_interrupt_clear(pio_instance, 0);
    pio_interrupt_clear(pio_instance, 1);
    irq_add_shared_handler(PIO0_IRQ_0, vga_line_irq,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_priority(PIO0_IRQ_0, 0x40);
    pio_set_irq0_source_enabled(pio_instance, pis_interrupt1, true);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_enable_sm_mask_in_sync(pio_instance, (1u << sm_sync) | (1u << sm_video));
}

void VGA_class::end() {
    pio_sm_set_enabled(pio_instance, sm_sync, false);
    pio_sm_set_enabled(pio_instance, sm_video, false);
    pio_set_irq0_source_enabled(pio_instance, pis_interrupt1, false);
    irq_remove_handler(PIO0_IRQ_0, vga_line_irq);
    if (dma_chan >= 0) {
        dma_channel_abort(dma_chan);
        dma_channel_unclaim(dma_chan);
        dma_chan = -1;
    }
    pio_sm_unclaim(pio_instance, sm_video);
    pio_sm_unclaim(pio_instance, sm_sync);
    pio_remove_program(pio_instance, &vga_out_program, pio_offset);
    pio_remove_program(pio_instance, &vga_sync_program, sync_offset);
}

void VGA_class::initGPIO() {
    gpio_init(VGA_PIN_HSYNC);
    gpio_set_dir(VGA_PIN_HSYNC, GPIO_OUT);
    gpio_put(VGA_PIN_HSYNC, true);
    pio_gpio_init(pio_instance, VGA_PIN_HSYNC);

    gpio_init(VGA_PIN_VSYNC);
    gpio_set_dir(VGA_PIN_VSYNC, GPIO_OUT);
    gpio_put(VGA_PIN_VSYNC, true);

    // Prepare Video pin for PIO control
    pio_gpio_init(pio_instance, VGA_PIN_VIDEO);
}

void VGA_class::initPIO() {
    sm_video = pio_claim_unused_sm(pio_instance, true);
    sm_sync = pio_claim_unused_sm(pio_instance, true);
    vga_out_instructions[0] = pio_encode_pull(false, true);
    vga_out_instructions[1] = pio_encode_out(pio_x, 32);
    vga_out_instructions[2] = pio_encode_wait_irq(true, false, 0);
    vga_out_instructions[3] = pio_encode_out(pio_pins, 1);
    vga_out_instructions[4] = pio_encode_jmp_x_dec(3);
    vga_out_instructions[5] = pio_encode_set(pio_pins, 0);
    vga_out_instructions[6] = pio_encode_jmp(0);
    pio_offset = pio_add_program(pio_instance, &vga_out_program);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, pio_offset, pio_offset + 6);
    
    // Configure OUT pin for 1-bit video
    sm_config_set_out_pins(&c, VGA_PIN_VIDEO, 1);
    sm_config_set_set_pins(&c, VGA_PIN_VIDEO, 1);
    
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // Each output bit takes two PIO instructions: 126 MHz / 2.5 / 2 = 25.2 MHz.
    sm_config_set_clkdiv(&c, 2.5f);

    pio_sm_init(pio_instance, sm_video, pio_offset, &c);
    pio_sm_set_consecutive_pindirs(pio_instance, sm_video, VGA_PIN_VIDEO, 1, true);
    // 800 pixel-clock ticks per line: sync 96, back porch 48,
    // active 640, front porch 16. PIO timing is independent of CPU IRQ latency.
    vga_sync_instructions[0] = pio_encode_set(pio_pins, 0);
    vga_sync_instructions[1] = pio_encode_irq_set(false, 1);
    vga_sync_instructions[2] = pio_encode_set(pio_x, 2);
    vga_sync_instructions[3] = pio_encode_jmp_x_dec(3) | pio_encode_delay(30);
    vga_sync_instructions[4] = pio_encode_set(pio_pins, 1) | pio_encode_delay(31);
    vga_sync_instructions[5] = pio_encode_nop() | pio_encode_delay(14);
    vga_sync_instructions[6] = pio_encode_irq_set(false, 0);
    vga_sync_instructions[7] = pio_encode_set(pio_x, 19) | pio_encode_delay(19);
    vga_sync_instructions[8] = pio_encode_jmp_x_dec(8) | pio_encode_delay(30);
    vga_sync_instructions[9] = pio_encode_nop() | pio_encode_delay(15);
    sync_offset = pio_add_program(pio_instance, &vga_sync_program);
    pio_sm_config sync = pio_get_default_sm_config();
    sm_config_set_wrap(&sync, sync_offset, sync_offset + 9);
    sm_config_set_set_pins(&sync, VGA_PIN_HSYNC, 1);
    sm_config_set_clkdiv(&sync, 5.0f);
    pio_sm_init(pio_instance, sm_sync, sync_offset, &sync);
    pio_sm_set_consecutive_pindirs(pio_instance, sm_sync, VGA_PIN_HSYNC, 1, true);
}

void VGA_class::initDMA() {
    dma_chan = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_high_priority(&c, true);
    
    // Trigger DMA when PIO TX FIFO has room
    channel_config_set_dreq(&c, pio_get_dreq(pio_instance, sm_video, true));

    dma_channel_configure(
        dma_chan,
        &c,
        &pio_instance->txf[sm_video], // Write target: PIO TX FIFO
        nullptr,                       // Read address set dynamically per scanline
        1 + VGA_BYTES_PER_LINE / 4,
        false                          // Don't start immediately
    );
}