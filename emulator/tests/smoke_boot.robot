*** Settings ***
Library                           Collections

*** Variables ***
# Override both with -v when invoking renode-test, e.g.:
#   FB_ADDR=$(nm ../../f4hwn.custom | awk '$3=="gFrameBuffer"{print "0x"$1}')
#   renode-test smoke_boot.robot -v FIRMWARE_ELF:../../f4hwn.custom -v FRAMEBUFFER_ADDR:$FB_ADDR
${FIRMWARE_ELF}                   ../../f4hwn.custom
${FRAMEBUFFER_ADDR}               0x20001376
${REPL}                           ${CURDIR}/../.generated/uvk5.repl

*** Test Cases ***
Firmware Boots And Draws A Screen
    # Regression guard against reintroducing an unbounded-poll boot hang
    # (see the two hazards documented in emulator/README.md): if boot never
    # gets past BOARD_Init(), gFrameBuffer stays all-zero (fresh .bss)
    # forever, since nothing ever calls ST7565_BlitFullScreen(). Getting
    # *any* nonzero byte in there is a strong, cheap signal that boot
    # actually reached the main loop and drew the welcome screen.
    # No explicit "start" here -- renode-test's harness already starts the
    # emulation once a machine exists, and issuing "start" again fails with
    # "This action is not available when emulation is already started".
    # `emulation RunFor` runs/advances it regardless of whether it was
    # already running (see Renode's own example robot tests, e.g.
    # tests/platforms/ambiq-apollo4.robot, which never call "start" either).
    Execute Command                mach create "uvk5-smoke-test"
    Execute Command                machine LoadPlatformDescription @${REPL}
    Execute Command                sysbus LoadELF @${FIRMWARE_ELF}

    Execute Command                emulation RunFor "5"

    ${bytes}=                      Execute Command    python "print(','.join(str(b) for b in self.Machine.SystemBus.ReadBytes(${FRAMEBUFFER_ADDR}, 896)))"
    ${found_nonzero}=              Run Keyword And Return Status    Should Not Match Regexp    ${bytes}    ^0(,0)*\\s*$
    Should Be True                 ${found_nonzero}    msg=gFrameBuffer is still all-zero after 5s -- boot likely hung before drawing anything
