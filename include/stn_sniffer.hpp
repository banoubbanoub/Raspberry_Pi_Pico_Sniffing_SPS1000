#ifndef STN_SNIFFER_HPP
#define STN_SNIFFER_HPP

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include <cstring>

constexpr uint16_t DISPLAY_WIDTH = 320;
constexpr uint16_t DISPLAY_HEIGHT = 240;
constexpr uint16_t CLOCKS_PER_LINE = DISPLAY_WIDTH / 4; // 80 bytes per scanline (4 pixels per byte)
constexpr uint16_t STORE_CLOCKS = CLOCKS_PER_LINE + 4;
constexpr uint16_t TOTAL_SCAN_LINES = DISPLAY_HEIGHT;
constexpr uint PIN_DATA_BASE = 0;
constexpr uint PIN_XSCL = 4;
constexpr uint PIN_LP = 8;
constexpr uint PIN_YD = 9;

class STNSniffer {
public:
    static STNSniffer& instance() {
        static STNSniffer inst;
        return inst;
    }

    void init();
    void start();
    void stop();

    void handleHsync();
    void handleVsync();
    void setSamplePhase(bool falling, uint8_t delay_cycles);

    const uint8_t* getFrameBuffer(uint32_t* sequence = nullptr) {
        const uint32_t interrupts = save_and_disable_interrupts();
        reading_ = true;
        const uint8_t* buffer = front_buffer_;
        if (sequence) *sequence = frame_seq_;
        restore_interrupts(interrupts);
        return buffer;
    }
    void releaseFrameBuffer() {
        const uint32_t interrupts = save_and_disable_interrupts();
        reading_ = false;
        restore_interrupts(interrupts);
    }

    uint32_t goodLines() const { return good_lines_; }
    uint32_t badLines() const { return bad_lines_; }
    uint32_t clockHist(uint8_t i) const { return hist_[i]; }
    uint32_t frameLp() const { return frame_lp_; }
    uint32_t frameGood() const { return frame_good_; }
    uint32_t frameSequence() const { return frame_seq_; }
    uint32_t lpEdges() const { return lp_edges_; }
    uint32_t ydEdges() const { return yd_edges_; }
    uint16_t lastClocks() const { return last_clocks_; }
    uint dmaChannel() const { return dma_chan_; }
    uint reloadDmaChannel() const { return dma_reload_chan_; }

    uint32_t measureScore(uint8_t frames);

private:
    STNSniffer() = default;
    ~STNSniffer() = default;
    STNSniffer(const STNSniffer&) = delete;
    STNSniffer& operator=(const STNSniffer&) = delete;

    uint8_t buffer_a_[STORE_CLOCKS * TOTAL_SCAN_LINES];
    uint8_t buffer_b_[STORE_CLOCKS * TOTAL_SCAN_LINES];

    static constexpr uint16_t RING_SIZE = 1024;
    // RP2040 hardware circular DMA requires naturally aligned ring buffer sizes (power of 2)
    uint8_t __attribute__((aligned(RING_SIZE))) ring_[RING_SIZE];

    uint8_t* front_buffer_ = buffer_a_;
    uint8_t* back_buffer_  = buffer_b_;
    volatile uint16_t current_line_ = 0;
    uint16_t prev_pos_ = 0;
    volatile uint32_t good_lines_ = 0;
    volatile uint32_t bad_lines_ = 0;
    volatile uint32_t hist_[7] = {};
    volatile uint32_t lp_count_ = 0, frame_lp_ = 0, frame_good_ = 0, good_at_vsync_ = 0;
    volatile uint32_t frame_seq_ = 0;
    volatile uint32_t lp_edges_ = 0;
    volatile uint32_t yd_edges_ = 0;
    volatile uint16_t last_clocks_ = 0;
    uint32_t line_hash_[TOTAL_SCAN_LINES];
    volatile bool reading_ = false;
    volatile bool capturing_ = true;
    uint8_t frame_count_ = 0;
    uint8_t program_offset_ = 0;
    bool pio_initialized_ = false;
    bool sample_falling_ = false;
    uint8_t sample_delay_ = 0;

    PIO pio_inst = pio1;
    uint sm_ = 0;
    uint dma_chan_ = -1;
    uint dma_reload_chan_ = -1;
    uint32_t dma_transfer_count_ = RING_SIZE;
};

#endif // STN_SNIFFER_HPP