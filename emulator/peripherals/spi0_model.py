# SPI0 (drives the ST7565 LCD over real hardware SPI). We don't need to decode
# what's actually sent -- the display viewer reads gFrameBuffer/gStatusLine
# directly out of RAM instead (see host_tools/display_viewer.py) -- but two
# specific status bits must always read a "ready" value or driver/st7565.c's
# init sequence spins forever:
#
#   FIFOST (offset 0x18) bit 4 (TFF, TX FIFO full) must always read 0 -- an
#   *unbounded* `while ((SPI0->FIFOST & TFF_MASK) != NOT_FULL) {}` in
#   ST7565_Init/DrawLine/etc would hang boot otherwise.
#
#   IF (offset 0x14) bit 5, polled by SPI_WaitForUndocumentedTxFifoStatusBit(),
#   is a *bounded* 100000-iteration loop that exits as soon as this bit reads
#   0 -- not a hang risk either way, but reading 0 immediately avoids wasting
#   100000 emulated loop iterations on every single byte sent to the LCD.
#
# CR/WDR/RDR are plain read-write scratch (nothing depends on their value).

OFFSET_FIFOST = 0x18
OFFSET_IF = 0x14

if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    if request.Offset == OFFSET_FIFOST:
        request.Value = 0
    elif request.Offset == OFFSET_IF:
        request.Value = 0
    else:
        request.Value = regs.get(request.Offset, 0)
