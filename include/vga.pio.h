.program vga_out
; Shifts out 1-bit monochrome video data at pixel clock frequency (25.2 MHz)
; Shifts MSB first out to the specified VIDEO_PIN.
.side_set 1 opt

.wrap_target
    pull block          ; Fetch 32-bit word from FIFO
    set x, 31           ; 32 bits per word
bit_loop:
    out pins, 1         ; Output 1 pixel bit
    jmp x-- bit_loop    ; Loop for all 32 bits
.wrap