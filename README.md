# Samsung SPS-1000 LCD Capture Using Raspberry Pi Pico (RP2040)

## Project Overview

This project reconstructs the live display of a **Samsung SPS-1000 cash register** using a Raspberry Pi Pico powered by the RP2040 microcontroller.

The original display interface is captured directly from the display controller signals. The RP2040 samples the 4-bit pixel data bus, synchronizes with the pixel clock and scan timing signals, and reconstructs the original monochrome display in real time.

The project demonstrates how a modern, inexpensive microcontroller can recover and reproduce the display output of vintage hardware without requiring the original display panel.

## Hardware

- Raspberry Pi Pico (RP2040)
- Samsung SPS-1000 cash register
- Original display controller: SED1351FOA
- Four-bit parallel pixel data bus
- XSCL pixel clock
- LP horizontal/line synchronization
- YD vertical/frame synchronization
- Connecting wires and suitable signal interfacing

## Signal Capture Architecture

The RP2040 uses its Programmable I/O (PIO) peripheral to capture the display data.

The capture pipeline consists of:

1. **PIO sampling:** Synchronizes with XSCL and samples the four-bit pixel data bus.
2. **DMA transfer:** Transfers captured samples from the PIO receive FIFO into a circular ring buffer.
3. **Line synchronization:** LP interrupts track scan lines and determine the amount of captured data.
4. **Frame synchronization:** YD interrupts track frame boundaries and reset the scan-line counter.
5. **Frame reconstruction:** Captured scan-line data is copied into a frame buffer.
6. **Frame-buffer management:** Front and back buffers are swapped when a complete frame has been captured successfully.

This architecture minimizes CPU involvement in individual pixel samples while allowing the processor to manage synchronization, buffering, and frame reconstruction.

## How It Works

The Samsung display controller provides the original display data as a four-bit parallel stream.

The RP2040 captures this stream using PIO and DMA, while the LP and YD signals provide the timing information required to organize the captured samples into scan lines and frames.

The resulting frame buffer represents the live contents of the Samsung SPS-1000 display.

Unlike a project that generates a fixed test pattern, this project captures the actual display signals produced by the running cash register.

## Why RP2040?

The RP2040 is particularly interesting for this application because it combines:

- Programmable I/O state machines for deterministic signal sampling.
- DMA transfers that reduce CPU intervention.
- Hardware-controlled GPIO and peripheral timing.
- On-chip SRAM for capture buffers.
- A compact, low-cost development board.

In this project, the RP2040 has produced a more stable reconstructed image than my earlier STM32F407 implementation under the tested conditions.

This demonstrates that peripheral architecture and deterministic sampling can matter as much as processor clock speed in real-time embedded capture applications.

## Project Goals

- Preserve and reconstruct a vintage cash register display.
- Explore real-time parallel digital signal acquisition.
- Learn how PIO, DMA, GPIO interrupts, and frame buffers work together.
- Develop a reusable approach for capturing similar legacy display interfaces.
- Experiment with alternative display output methods in future versions.

## Current Status

**Working prototype:** The Raspberry Pi Pico successfully captures the live Samsung SPS-1000 display signals, and the reconstructed image is stable and readable.

The capture implementation includes PIO sampling, circular DMA buffering, line tracking, frame synchronization, and front/back buffer management.

## Future Improvements

- Improve capture diagnostics and synchronization validation.
- Document the electrical interface and signal timing.
- Add additional output options, such as VGA or browser-based display streaming.
- Adapt the capture architecture to other legacy displays that expose similar digital signals.

## Important Notes

This project captures digital display signals directly from vintage hardware. Verify signal voltage levels and electrical compatibility before connecting a microcontroller. Use appropriate level shifting or buffering when required, and avoid loading the original display controller outputs.

Complete System Circuit Diagram
================================================================================
 1. POWER SUPPLY SECTION
================================================================================
Samsung SPS 1000                          Buck Converter (HW-045 / LM2596 type)
 [ 5V Pin ] ─────────────────────────────> [ VIN+ ]
 [ GND    ] ─────────────────────────────> [ VIN- ]

                                          [ VOUT+ ] ──+──> 3.3V Rail (Pico 3V3)
                                          [ VOUT- ] ──+──> Common Ground (GND)

================================================================================
 2. LEVEL SHIFTER SECTION (e.g., 20-Pin DIP 74LVC245 / 74HCT245)
================================================================================
SPS 1000 Signals (5V Logic)               Level Shifter IC                  Pico (3.3V Logic)
 [ Data 0 ] ─────────────────────────────> Pin 2 [A1] ──> [B1] Pin 18 ──────> GPIO 0
 [ Data 1 ] ─────────────────────────────> Pin 3 [A2] ──> [B2] Pin 17 ──────> GPIO 1
 [ Data 2 ] ─────────────────────────────> Pin 4 [A3] ──> [B3] Pin 16 ──────> GPIO 2
 [ Data 3 ] ─────────────────────────────> Pin 5 [A4] ──> [B4] Pin 15 ──────> GPIO 3
 [ XSCL   ] ─────────────────────────────> Pin 6 [A5] ──> [B5] Pin 14 ──────> GPIO 4
 [ LP     ] ─────────────────────────────> Pin 7 [A6] ──> [B6] Pin 13 ──────> GPIO 8
 [ YD     ] ─────────────────────────────> Pin 8 [A7] ──> [B7] Pin 12 ──────> GPIO 9

 * Power & Control Connections for Level Shifter:
   - Pin 20 [VCC]  ──> 5V Rail (from SPS 1000 / Buck VIN+)
   - Pin 10 [GND]  ──> Common Ground
   - Pin 1  [DIR]  ──> 5V Rail (Direction: A -> B)
   - Pin 19 [OE/]  ──> Common Ground (Output Enabled)
   - Low-side VCCA (if dual-supply like TXS0108E) ──> 3.3V Rail

================================================================================
 3. RASPBERRY PI PICO & VGA OUTPUT SECTION
================================================================================
Raspberry Pi Pico                         VGA DB15 Connector
 [ GPIO 16 ] ───────────────────────────> Pin 13 (HSYNC)
 [ GPIO 17 ] ───────────────────────────> Pin 14 (VSYNC)
 [ GPIO 18 ] ──[ 270Ω Resistor ]────────> Pin 2  (Green Video Out)

 [ GND     ] ──┬────────────────────────> Pins 5, 6, 7, 8, 10 (Analog Grounds)
               └────────────────────────> VGA Shell / Chassis Ground
================================================================================

## Conclusion

This project explores the intersection of retrocomputing, digital electronics, and embedded systems.

By combining the Samsung SPS-1000 display interface with the Raspberry Pi Pico's PIO and DMA capabilities, it demonstrates a practical way to reconstruct a live legacy display using modern microcontroller hardware.

**Old hardware. Modern microcontrollers. Real-time display reconstruction.**
