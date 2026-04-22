# Uracil stub — cross-compilation targets
#
# Build pre-compiled stubs (optional; ura_builder.py compiles on the fly):
#   make                   # native x86_64 ELF
#   make x86_64_win        # Windows PE via mingw
#   make riscv_elf         # RISC-V ELF
#   make all               # all three

CC      ?= gcc
CFLAGS  ?= -O2 -s -Wall -Wextra
SRC      = ura_stub.c

.PHONY: all clean x86_64_elf x86_64_win riscv_elf

all: x86_64_elf x86_64_win riscv_elf

x86_64_elf: $(SRC)
	$(CC) $(CFLAGS) -o ura_stub_x86_64_elf $(SRC) -static 2>/dev/null || \
	$(CC) $(CFLAGS) -o ura_stub_x86_64_elf $(SRC)

x86_64_win: $(SRC)
	x86_64-w64-mingw32-gcc $(CFLAGS) -o ura_stub_x86_64_win.exe $(SRC) -static

riscv_elf: $(SRC)
	riscv64-linux-gnu-gcc $(CFLAGS) -o ura_stub_riscv_elf $(SRC) -static

clean:
	rm -f ura_stub_x86_64_elf ura_stub_x86_64_win.exe ura_stub_riscv_elf
