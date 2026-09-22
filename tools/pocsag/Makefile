# Host-side build for the POCSAG encoder harness. Nothing here is part of the
# firmware build; it compiles app/pocsag.c natively so the encoder that runs on
# the radio and the one under test are the same source file.

TOP     := ../..
CC      ?= cc
# -MMD because these objects depend on headers in the firmware tree: without it
# a change to a struct leaves a stale object behind and the test aborts on an
# ABI mismatch rather than failing a check.
CFLAGS  += -std=c11 -O2 -Wall -Wextra -Wpedantic -MMD -I $(TOP)

BIN     := pocsag_wav
# built here rather than next to the source so the firmware tree stays clean
OBJS    := pocsag_wav.o pocsag.o

HWBIN   := hwfsk_test
HWOBJS  := hwfsk_test.o bk4819-hwfsk-stub.o pocsag.o
# the stub swaps SysTick and the chip for a simulated clock
FSKFLAGS := -DBK4819_FSK_HOST_TEST -I .

RXBIN   := rx_test
RXOBJS  := rx_test.o pocsag_rx.o pocsag.o
# the receive session needs the chip, the decoder does not, so leave it out
RXFLAGS := -DPOCSAG_RX_HOST_TEST

# multimon-ng is not in homebrew, build it from
# https://github.com/EliasOenal/multimon-ng and point MULTIMON at the binary
MULTIMON ?= multimon-ng

# multimon-ng only accepts 22050Hz input
RATE    := 22050
RIC     ?= 1234567
BAUD    ?= 1200
MSG     ?= HELLO FROM UV-K5

.PHONY: all test demo clean

all: $(BIN) $(HWBIN) $(RXBIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(HWBIN): $(HWOBJS)
	$(CC) $(CFLAGS) -o $@ $(HWOBJS)

$(RXBIN): $(RXOBJS)
	$(CC) $(CFLAGS) -o $@ $(RXOBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

pocsag.o: $(TOP)/app/pocsag.c
	$(CC) $(CFLAGS) -c $< -o $@

hwfsk_test.o: hwfsk_test.c
	$(CC) $(CFLAGS) $(FSKFLAGS) -c $< -o $@

bk4819-hwfsk-stub.o: $(TOP)/driver/bk4819-hwfsk.c
	$(CC) $(CFLAGS) $(FSKFLAGS) -c $< -o $@

pocsag_rx.o: $(TOP)/app/pocsag_rx.c
	$(CC) $(CFLAGS) $(RXFLAGS) -c $< -o $@

test: $(BIN) $(HWBIN) $(RXBIN)
	@echo "== PCD5002 decoder vs real hardware dump =="
	./fixture_test.sh
	@echo
	@echo "== hardware FSK modem =="
	./$(HWBIN)
	@echo
	@echo "== POCSAG receive decoder =="
	./$(RXBIN)
	@echo
	@echo "== POCSAG encoder =="
	MULTIMON=$(MULTIMON) ./roundtrip.sh

demo: $(BIN)
	./$(BIN) -r $(RIC) -b $(BAUD) -t alpha -f 3 -d -o pocsag.wav "$(MSG)"
	$(MULTIMON) -t wav -a POCSAG$(BAUD) -q pocsag.wav

clean:
	rm -f $(BIN) $(OBJS) $(HWBIN) $(HWOBJS) $(RXBIN) $(RXOBJS) pocsag.wav *.d

-include $(OBJS:.o=.d) $(HWOBJS:.o=.d) $(RXOBJS:.o=.d)
