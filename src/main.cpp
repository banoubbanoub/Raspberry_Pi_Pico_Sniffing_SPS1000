#include <Arduino.h>
#include "Engine3D.h"
#include "VGA.h"
#include "stn_sniffer.hpp"

Engine3D engine3D;

static constexpr uint32_t STN_PIN_MASK =
    (0x0FU << PIN_DATA_BASE) | (1U << PIN_XSCL) | (1U << PIN_LP) | (1U << PIN_YD);
static constexpr uint32_t VGA_PIN_MASK =
    (1U << VGA_PIN_HSYNC) | (1U << VGA_PIN_VSYNC) | (1U << VGA_PIN_VIDEO);
static_assert((STN_PIN_MASK & VGA_PIN_MASK) == 0,
              "STN input and VGA output pins must not overlap");

static constexpr uint16_t OUTPUT_WIDTH = DISPLAY_WIDTH * 5 / 4;
static constexpr uint16_t OUTPUT_HEIGHT = DISPLAY_HEIGHT * 5 / 4;
static constexpr uint16_t X_BYTE_OFF = (VGA_BYTES_PER_LINE - OUTPUT_WIDTH / 8) / 2;
static constexpr uint16_t Y_ROW_OFF = (VGA_HEIGHT - OUTPUT_HEIGHT) / 2 - 8;
static_assert(OUTPUT_WIDTH % 8 == 0 && OUTPUT_WIDTH <= VGA_WIDTH,
              "Captured image must fit the VGA byte stride");
static_assert(Y_ROW_OFF + OUTPUT_HEIGHT <= VGA_HEIGHT,
              "Captured image must fit the VGA frame");

static uint8_t capturedFrame[STORE_CLOCKS * DISPLAY_HEIGHT];
static uint8_t expandedNibble[16];
static uint32_t renderedSequence = 0;

static void renderSniffer() {
    STNSniffer& sniffer = STNSniffer::instance();
    if (dma_channel_hw_addr(sniffer.dmaChannel())->ctrl_trig &
        (DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS)) {
        Serial.println("ERROR: STN DMA bus fault");
        panic("STN DMA bus fault");
    }
    if (sniffer.frameSequence() == renderedSequence) return;

    const uint8_t* source = sniffer.getFrameBuffer(&renderedSequence);
    memcpy(capturedFrame, source, sizeof(capturedFrame));
    sniffer.releaseFrameBuffer();
    uint8_t* destination = engine3D.getBackBuffer();

    uint16_t previousSourceY = DISPLAY_HEIGHT;
    for (uint16_t y = 0; y < OUTPUT_HEIGHT; ++y) {
        const uint16_t sourceY = y * DISPLAY_HEIGHT / OUTPUT_HEIGHT;
        const uint8_t* line = capturedFrame + sourceY * STORE_CLOCKS +
                              STORE_CLOCKS - CLOCKS_PER_LINE;
        uint8_t* out = destination + (Y_ROW_OFF + y) * VGA_BYTES_PER_LINE + X_BYTE_OFF;
        if (sourceY == previousSourceY) {
            memcpy(out, out - VGA_BYTES_PER_LINE, OUTPUT_WIDTH / 8);
            continue;
        }
        previousSourceY = sourceY;
        // Eight four-pixel samples expand to exactly five packed VGA bytes.
        for (uint16_t sample = 0; sample < CLOCKS_PER_LINE; sample += 8) {
            const uint8_t a = expandedNibble[line[sample] & 15];
            const uint8_t b = expandedNibble[line[sample + 1] & 15];
            const uint8_t c = expandedNibble[line[sample + 2] & 15];
            const uint8_t d = expandedNibble[line[sample + 3] & 15];
            const uint8_t e = expandedNibble[line[sample + 4] & 15];
            const uint8_t f = expandedNibble[line[sample + 5] & 15];
            const uint8_t g = expandedNibble[line[sample + 6] & 15];
            const uint8_t h = expandedNibble[line[sample + 7] & 15];
            *out++ = (a << 3) | (b >> 2);
            *out++ = (b << 6) | (c << 1) | (d >> 4);
            *out++ = (d << 4) | (e >> 1);
            *out++ = (e << 7) | (f << 2) | (g >> 3);
            *out++ = (g << 5) | h;
        }
    }
    engine3D.display();
}

void setup() {
    Serial.begin(115200);
    engine3D.begin();
    if (!engine3D.setDoubleBuffering(true)) {
        Serial.println("ERROR: VGA back buffer allocation failed");
        panic("VGA back buffer allocation failed");
    }
    for (uint8_t nibble = 0; nibble < 16; ++nibble) {
        const uint8_t pixels = static_cast<uint8_t>(~nibble) & 15;
        expandedNibble[nibble] = ((pixels & 8) << 1) | pixels;
    }

    STNSniffer& sniffer = STNSniffer::instance();
    sniffer.init();
    if (sniffer.dmaChannel() == static_cast<uint>(VGA.dmaChannel()) ||
        sniffer.reloadDmaChannel() == static_cast<uint>(VGA.dmaChannel()) ||
        sniffer.dmaChannel() == sniffer.reloadDmaChannel()) {
        Serial.println("ERROR: STN/VGA DMA channel conflict");
        panic("STN/VGA DMA channel conflict");
    }
    Serial.print("VGA DMA=");
    Serial.print(VGA.dmaChannel());
    Serial.print(" STN DMA=");
    Serial.print(sniffer.dmaChannel());
    Serial.print(" reload DMA=");
    Serial.println(sniffer.reloadDmaChannel());
    sniffer.setSamplePhase(true, 1);
    sniffer.start();
}

void loop() {
    renderSniffer();
}
