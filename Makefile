# EmbedForge — host tests and Cortex-M firmware from one Makefile.
#
#   make test        build and run the host unit tests (native cc)
#   make test-san    the same tests under AddressSanitizer + UBSan
#   make firmware    link the ARM image and print its footprint
#   make size        footprint table only
#   make qemu        run the QEMU integration tests
#   make cppcheck    static analysis over core/, bsp/, app/
#   make no-heap     prove the image contains no dynamic allocation
#   make all         everything above, in the order CI runs it
#
# The ARM half is skipped with a clear message when arm-none-eabi-gcc is not
# installed, so `make test` works on any machine.

BUILD    := build
CORE_DIR := core
TEST_DIR := tests

# ---- host tests -------------------------------------------------------------

HOST_CC     ?= cc
HOST_WARN   := -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
               -Wsign-conversion -Wstrict-prototypes -Wpointer-arith \
               -Wcast-qual -Wundef
HOST_CFLAGS := $(HOST_WARN) -O2 -g -I$(CORE_DIR) -I$(TEST_DIR)
SAN_FLAGS   := -fsanitize=address,undefined -fno-sanitize-recover=all -O1

CORE_SRC := $(CORE_DIR)/ringbuf.c $(CORE_DIR)/parser.c $(CORE_DIR)/filter.c \
            $(CORE_DIR)/sched.c $(CORE_DIR)/fmt.c

TESTS    := ringbuf parser filter sched fmt fuzz
TEST_BIN := $(addprefix $(BUILD)/test_,$(TESTS))
SAN_BIN  := $(addprefix $(BUILD)/san_,$(TESTS))

$(BUILD)/test_%: $(TEST_DIR)/test_%.c $(CORE_SRC) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(CORE_SRC)

$(BUILD)/san_%: $(TEST_DIR)/test_%.c $(CORE_SRC) | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(SAN_FLAGS) -o $@ $< $(CORE_SRC)

.PHONY: test
test: $(TEST_BIN)
	@echo "== host unit tests =="
	@for t in $(TEST_BIN); do $$t || exit 1; done
	@echo "== all host tests passed =="

.PHONY: test-san
test-san: $(SAN_BIN)
	@echo "== host unit tests under ASan + UBSan =="
	@for t in $(SAN_BIN); do $$t || exit 1; done
	@echo "== all sanitizer runs clean =="

# ---- ARM firmware -----------------------------------------------------------

ARM_CC   := arm-none-eabi-gcc
ARM_SIZE := arm-none-eabi-size
ARM_OBJDUMP := arm-none-eabi-objdump
ARM_NM   := arm-none-eabi-nm
HAVE_ARM := $(shell command -v $(ARM_CC) 2>/dev/null)

# -ffreestanding: no assumptions about a hosted environment.
# -nostdlib: no C library at all in the image (see bsp/libc_shim.c).
# -ffunction-sections/-fdata-sections + --gc-sections: drop anything unreached,
#   so the footprint number reflects what actually runs.
ARM_ARCH   := -mcpu=cortex-m3 -mthumb
ARM_CFLAGS := $(ARM_ARCH) -std=c11 -Os -g3 \
              -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
              -Wstrict-prototypes -Wpointer-arith -Wcast-qual -Wundef \
              -ffreestanding -fno-common -ffunction-sections -fdata-sections \
              -I$(CORE_DIR) -Ibsp
ARM_LDFLAGS := $(ARM_ARCH) -nostdlib -Wl,--gc-sections \
               -T bsp/linker.ld -Wl,-Map=$(BUILD)/firmware.map \
               -Wl,--cref -Wl,--print-memory-usage

FW_SRC := app/main.c \
          $(CORE_SRC) \
          bsp/bsp.c bsp/uart.c bsp/systick.c bsp/gpio.c bsp/startup.c \
          bsp/libc_shim.c

ELF := $(BUILD)/firmware.elf

$(ELF): $(FW_SRC) bsp/linker.ld | $(BUILD)
ifndef HAVE_ARM
	@echo "SKIP: $(ARM_CC) not found. Install it to build the firmware:"
	@echo "      brew install arm-none-eabi-gcc     (macOS)"
	@echo "      apt-get install gcc-arm-none-eabi  (Debian/Ubuntu)"
	@exit 1
endif
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) -o $@ $(FW_SRC) -lgcc

.PHONY: firmware
firmware: $(ELF) size

.PHONY: size
size: $(ELF)
	@echo ""
	@python3 tools/footprint.py $(ELF) $(ARM_SIZE)

# ---- the no-heap guarantee --------------------------------------------------
#
# Three independent checks, because "we don't call malloc" is a claim and this
# turns it into a property of the artifact:
#   1. no allocation call appears in the portable sources,
#   2. no allocator symbol survives into the linked image,
#   3. no _sbrk / heap symbol exists for one to grow into.

.PHONY: no-heap
no-heap: $(ELF)
	@echo "== no-heap check =="
	@if grep -nE '\b(malloc|calloc|realloc|free|strdup|alloca)\s*\(' \
	    $(CORE_DIR)/*.c $(CORE_DIR)/*.h app/*.c bsp/*.c bsp/*.h ; then \
	    echo "FAIL: dynamic allocation referenced in source"; exit 1; \
	fi
	@echo "  source: no allocation calls in core/, app/ or bsp/"
	@if $(ARM_NM) $(ELF) | grep -E ' (T|t|W|w|U) (malloc|free|calloc|realloc|_sbrk|_malloc_r)$$' ; then \
	    echo "FAIL: allocator symbol linked into the image"; exit 1; \
	fi
	@echo "  image:  no allocator symbols in firmware.elf"
	@if $(ARM_NM) $(ELF) | grep -qE ' (end|__heap_start|__HeapBase)$$' ; then \
	    echo "FAIL: a heap region symbol exists"; exit 1; \
	fi
	@echo "  link:   no heap region defined by the linker script"
	@echo "  => the image cannot allocate: there is no allocator and no heap"

# ---- static analysis --------------------------------------------------------

.PHONY: cppcheck
cppcheck:
	@echo "== cppcheck =="
	cppcheck --error-exitcode=1 --enable=warning,style,performance,portability \
	         --inline-suppr --std=c11 --quiet \
	         --suppress=missingIncludeSystem \
	         --suppress=unusedStructMember \
	         --suppress=checkersReport \
	         -I $(CORE_DIR) -I bsp \
	         $(CORE_DIR) bsp app

# ---- QEMU -------------------------------------------------------------------

.PHONY: qemu
qemu: $(ELF)
	python3 qemu/run_qemu.py --elf $(ELF)

.PHONY: qemu-record
qemu-record: $(ELF)
	python3 qemu/run_qemu.py --elf $(ELF) --record

# ---- housekeeping -----------------------------------------------------------

$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: all
all: test test-san cppcheck firmware no-heap qemu

.PHONY: clean
clean:
	rm -rf $(BUILD)
