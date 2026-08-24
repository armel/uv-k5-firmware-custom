# Generic scratch register bank for peripherals the firmware only needs to not
# crash/hang on: writes are stored, reads return whatever was last written (0
# for anything never written). No side effects, no protocol logic.
#
# Reused (via separate `filename:` peripheral instances, each with its own
# independent state) for SYSCON, PORTCON, GPIOB, and UART1 -- see
# emulator/README.md for why each of those is safe to treat this simply.

if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    request.Value = regs.get(request.Offset, 0)
