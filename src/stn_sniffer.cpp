#include "stn_sniffer.hpp"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio_instructions.h"
#include "pico/time.h"

/*
 * STN capture pipeline: GPIO data -> PIO1 RX FIFO -> DMA ring -> frame buffers.
 * GP0-GP3 carry four pixels per XSCL sample; GP4 is XSCL.
 * GP8 LP rising edges delimit rows; GP9 YD falling edges delimit frames.
 * PIO samples four bits and autopushes each nibble into the RX FIFO.
 * Capture DMA stores one nibble per byte in a naturally aligned circular ring.
 * A second DMA channel reloads the capture count after each ring completes.
 * LP interrupts copy recent samples into the back frame buffer.
 * YD interrupts publish a completed-height frame unless the reader holds it.
 * VGA output runs separately on PIO0 and consumes the captured image in main.
 */

#ifndef STN_FRAME_DIV
#define STN_FRAME_DIV 1
#endif

static uint16_t stn_sniff_instructions[4];
static const pio_program stn_sniff_program = {
    stn_sniff_instructions,
    4,
    -1
};

static STNSniffer* g_sniffer = nullptr;

static void gpio_callback(uint gpio, uint32_t events) {
    if (!g_sniffer) return;

    if (gpio == PIN_LP && (events & GPIO_IRQ_EDGE_RISE)) {
        g_sniffer->handleHsync();
    } else if (gpio == PIN_YD && (events & GPIO_IRQ_EDGE_FALL)) {
        g_sniffer->handleVsync();
    }
}

void STNSniffer::init() {
    g_sniffer = this;

    // 1. Configure data pins as PIO inputs
    for (uint i = 0; i < 4; ++i) {
        gpio_init(PIN_DATA_BASE + i);
        gpio_set_dir(PIN_DATA_BASE + i, GPIO_IN);
        gpio_pull_down(PIN_DATA_BASE + i);
        pio_gpio_init(pio_inst, PIN_DATA_BASE + i);
    }

    // 2. Configure Sync Pins (LP on GPIO 8, YD on GPIO 9)
    gpio_init(PIN_LP);
    gpio_set_dir(PIN_LP, GPIO_IN);
    gpio_pull_up(PIN_LP);

    gpio_init(PIN_YD);
    gpio_set_dir(PIN_YD, GPIO_IN);
    gpio_pull_up(PIN_YD);

    // Capture one 4-bit sample for each XSCL clock.
    stn_sniff_instructions[0] = pio_encode_wait_gpio(true, PIN_XSCL);
    stn_sniff_instructions[1] = pio_encode_in(pio_pins, 4);
    stn_sniff_instructions[2] = pio_encode_wait_gpio(false, PIN_XSCL);
    stn_sniff_instructions[3] = pio_encode_jmp(0);
    program_offset_ = static_cast<uint8_t>(pio_add_program(pio_inst, &stn_sniff_program));
    sm_ = pio_claim_unused_sm(pio_inst, true);
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_in_pins(&c, PIN_DATA_BASE);
    // Left shifting places the four input bits in the low nibble. Autopush
    // sends each sample to RX FIFO; byte-sized DMA reads retain that nibble.
    sm_config_set_in_shift(&c, false, true, 4);
    sm_config_set_wrap(&c, program_offset_, program_offset_ + 3);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio_inst, sm_, program_offset_, &c);
    pio_gpio_init(pio_inst, PIN_XSCL);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_, PIN_DATA_BASE, 4, false);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_, PIN_XSCL, 1, false);
    pio_initialized_ = true;
    setSamplePhase(sample_falling_, sample_delay_);

    // 4. Setup Circular DMA channel to pull from PIO FIFO into ring buffer
    dma_chan_ = dma_claim_unused_channel(true);
    dma_reload_chan_ = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan_);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    // Hardware wraps the low address bits, so the ring must have a power-of-two
    // size and its base address must be aligned to that same size.
    channel_config_set_ring(&dc, true, __builtin_ctz(RING_SIZE)); 
    channel_config_set_dreq(&dc, pio_get_dreq(pio_inst, sm_, false));
    channel_config_set_chain_to(&dc, dma_reload_chan_);

    dma_channel_configure(
        dma_chan_,
        &dc,
        ring_,                  // Destination ring buffer
        &pio_inst->rxf[sm_],    // Source: PIO RX FIFO
        RING_SIZE,
        false
    );

    // Reload and trigger capture after every ring without depending on LP IRQs.
    dma_channel_config reload = dma_channel_get_default_config(dma_reload_chan_);
    channel_config_set_transfer_data_size(&reload, DMA_SIZE_32);
    channel_config_set_read_increment(&reload, false);
    channel_config_set_write_increment(&reload, false);
    // Writing this alias reloads TRANS_COUNT and triggers the capture channel.
    // The fixed read/write addresses let the reload channel repeat on chaining.
    dma_channel_configure(
        dma_reload_chan_, &reload,
        &dma_channel_hw_addr(dma_chan_)->al1_transfer_count_trig,
        &dma_transfer_count_, 1, false);

    // Clear buffers
    memset(buffer_a_, 0, sizeof(buffer_a_));
    memset(buffer_b_, 0, sizeof(buffer_b_));
}

void STNSniffer::start() {
    current_line_ = 0;
    prev_pos_ = 0;
    
    // Start DMA and PIO state machine
    dma_channel_config capture = dma_get_channel_config(dma_chan_);
    channel_config_set_chain_to(&capture, dma_reload_chan_);
    dma_channel_set_config(dma_chan_, &capture, false);
    dma_channel_set_write_addr(dma_chan_, ring_, false);
    dma_channel_set_trans_count(dma_chan_, RING_SIZE, false);
    pio_sm_clear_fifos(pio_inst, sm_);
    pio_sm_restart(pio_inst, sm_);
    pio_sm_exec(pio_inst, sm_, pio_encode_jmp(program_offset_));
    // Arm DMA first so the initial PIO samples already have a FIFO consumer.
    dma_channel_start(dma_chan_);
    pio_sm_set_enabled(pio_inst, sm_, true);

    // Enable GPIO Interrupts for HSYNC (Rising) and VSYNC (Falling)
    irq_set_priority(IO_IRQ_BANK0, 0x80);
    gpio_set_irq_enabled_with_callback(PIN_LP, GPIO_IRQ_EDGE_RISE, true, &gpio_callback);
    gpio_set_irq_enabled(PIN_YD, GPIO_IRQ_EDGE_FALL, true);
}

void STNSniffer::stop() {
    pio_sm_set_enabled(pio_inst, sm_, false);
    // Chaining to the same channel disables chaining, preventing capture from
    // starting another reload while the two channels are being aborted.
    dma_channel_config capture = dma_get_channel_config(dma_chan_);
    channel_config_set_chain_to(&capture, dma_chan_);
    dma_channel_set_config(dma_chan_, &capture, false);
    dma_channel_abort(dma_reload_chan_);
    dma_channel_abort(dma_chan_);
    gpio_set_irq_enabled(PIN_LP, GPIO_IRQ_EDGE_RISE, false);
    gpio_set_irq_enabled(PIN_YD, GPIO_IRQ_EDGE_FALL, false);
}

void STNSniffer::handleHsync() {
    const uint16_t mask = RING_SIZE - 1;
    
    dma_channel_hw_t* channel_hw = dma_channel_hw_addr(dma_chan_);
    const uint16_t pos = static_cast<uint16_t>(
        static_cast<uintptr_t>(channel_hw->write_addr) - reinterpret_cast<uintptr_t>(ring_)) & mask;

    // Masked subtraction also works when the DMA write pointer wraps.
    // This is the position at IRQ service time, not a hardware-latched LP position.
    uint16_t clocks = (pos - prev_pos_) & mask;
    last_clocks_ = clocks;
    lp_edges_++;
    prev_pos_ = pos;
    lp_count_++;

    hist_[clocks < CLOCKS_PER_LINE ? 0 : (clocks <= STORE_CLOCKS ? clocks - CLOCKS_PER_LINE + 1 : 6)]++;

    if (current_line_ >= TOTAL_SCAN_LINES) return;

    if (capturing_) {
        // Accept 80..84 samples: 80 four-bit groups cover the 320-pixel row.
        // Store the most recent 84; main displays the final 80 groups.
        if (clocks >= CLOCKS_PER_LINE && clocks <= STORE_CLOCKS) {
            const uint16_t src = (pos - STORE_CLOCKS) & mask;
            uint8_t* dst = back_buffer_ + (current_line_ * STORE_CLOCKS);

            for (uint16_t i = 0; i < STORE_CLOCKS; ++i) {
                dst[i] = ring_[(src + i) & mask];
            }
            good_lines_++;
        } else {
            // Leave this row's previous back-buffer contents unchanged.
            bad_lines_++;
        }
    }
    current_line_++;
}

void STNSniffer::setSamplePhase(bool falling, uint8_t delay_cycles) {
    // Main selects the hardware-tested falling edge with one delay cycle.
    // Delay units are PIO clocks, not microseconds (PIO divider is 1 here).
    if (delay_cycles > 31) {
        panic("STN sample delay must be between 0 and 31 PIO cycles");
    }

    sample_falling_ = falling;
    sample_delay_ = delay_cycles;
    if (!pio_initialized_) return;

    const bool was_enabled = (pio_inst->ctrl & (1u << sm_)) != 0;
    pio_sm_set_enabled(pio_inst, sm_, false);

    // Delay after the selected edge, before sampling rather than after IN.
    pio_inst->instr_mem[program_offset_] =
        pio_encode_wait_gpio(!falling, PIN_XSCL) |
        (static_cast<uint16_t>(delay_cycles) << 8);
    pio_inst->instr_mem[program_offset_ + 1] =
        pio_encode_in(pio_pins, 4);
    pio_inst->instr_mem[program_offset_ + 2] =
        pio_encode_wait_gpio(falling, PIN_XSCL);
    // Use the loaded program's address, since it need not begin at address zero.
    pio_inst->instr_mem[program_offset_ + 3] =
        pio_encode_jmp(program_offset_);

    if (was_enabled) {
        // Restart at the selected edge instead of resuming mid-sample.
        pio_sm_clear_fifos(pio_inst, sm_);
        pio_sm_restart(pio_inst, sm_);
        pio_sm_exec(pio_inst, sm_, pio_encode_jmp(program_offset_));
        pio_sm_set_enabled(pio_inst, sm_, true);
    }
}

void STNSniffer::handleVsync() {
    yd_edges_++;
    // Reaching 240 LP rows establishes frame height, not validity of every row.
    const bool frame_complete = current_line_ >= TOTAL_SCAN_LINES;
    frame_lp_ = lp_count_;
    frame_good_ = good_lines_ - good_at_vsync_;
    good_at_vsync_ = good_lines_;
    lp_count_ = 0;
    current_line_ = 0;

    const bool was_capturing = capturing_;
    // With divisor 1 every frame is captured; larger divisors skip whole frames.
    if (++frame_count_ >= STN_FRAME_DIV) {
        frame_count_ = 0;
        capturing_ = true;
    } else {
        capturing_ = false;
    }

    // Never swap while main is reading the front buffer. Capture continues into
    // the back buffer, but publication is skipped for this frame.
    if (!frame_complete || !was_capturing || reading_) return;

    uint8_t* temp = front_buffer_;
    front_buffer_ = back_buffer_;
    back_buffer_  = temp;
    frame_seq_++; // Lets the renderer detect a newly published capture.
}

uint32_t STNSniffer::measureScore(uint8_t frames) {
    // Calibration helper for a static source: count changed row hashes and bad
    // rows. Lower scores indicate repeatable data, not necessarily correct data.
    uint32_t score = 0;
    const uint32_t bad0 = bad_lines_;
    uint32_t seen = frame_seq_;
    bool have_prev = false;

    // The first frame seeds the hashes; subsequent frames contribute comparisons.
    for (uint8_t n = 0; n <= frames; ++n) {
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        while (frame_seq_ == seen) {
            // A missing input or unpublished frame must not hang calibration.
            if (to_ms_since_boot(get_absolute_time()) - t0 > 200) return 0xFFFFFFFFu;
        }
        seen = frame_seq_;

        reading_ = true;
        const uint8_t* p = front_buffer_;
        for (uint16_t y = 0; y < TOTAL_SCAN_LINES; ++y) {
            uint32_t h = 2166136261u;
            for (uint16_t i = 0; i < STORE_CLOCKS; ++i) {
                h = (h ^ p[y * STORE_CLOCKS + i]) * 16777619u;
            }
            if (have_prev && h != line_hash_[y]) score++;
            line_hash_[y] = h;
        }
        reading_ = false;
        have_prev = true;
    }

    return score + (bad_lines_ - bad0);
}