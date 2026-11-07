# libNovaC build.
#
#   make host-test      portable-core unit tests on the host (ASan+UBSan, fake kernel)
#   make lib            build/target/libNovaC.a + crt0.o for NovarisOS (x86-64)
#   make target-test    build the freestanding test program and run it on this
#                       Linux host (same ABI NovarisOS implements)
#   make check-sysnum   verify syscall numbers against ../../include/syscall64.h
#   make example        compile-check examples/demo.c
#
# The target is x86-64 with the Linux syscall ABI. NOVAC_CC must target x86-64;
# the build refuses anything else so it cannot silently use a wrong compiler.
# Sources are compiled against libNovaC's own headers plus the compiler's
# freestanding headers; host libc headers are excluded with -nostdinc.

NOVAC_CC  ?= gcc
NOVAC_AR  ?= ar
NOVAC_LD  ?= ld
HOSTCC    ?= cc
BUILD     ?= build
OUT       ?= $(BUILD)/target/novac_target_test.elf

PORTABLE_SRC := $(wildcard src/string/*.c src/ctype/*.c src/errno/*.c src/assert/*.c src/stdlib/*.c) \
                src/stdio/format.c src/stdio/snprintf.c
SYSTEM_SRC   := src/dirent/dirent.c src/stdio/file.c src/unistd/unistd.c src/time/time.c src/sys/novaris/sys.c \
                src/signal/signal.c
CRT_SRC      := src/arch/x86_64/start.c
LIB_SRC      := $(PORTABLE_SRC) $(SYSTEM_SRC)

cc-option = $(shell $(1) $(2) -x c -c /dev/null -o /dev/null 2>/dev/null && echo $(2))
WARN := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes

.PHONY: lib child check-target host-test target-test check-sysnum example clean

# ---- NovarisOS target ------------------------------------------------------
NOVAC_BUILTIN_INC ?= $(shell $(NOVAC_CC) -print-file-name=include 2>/dev/null)
TGT_FLAGS = -std=c11 -m64 -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-pic -O2 $(WARN) \
            $(call cc-option,$(NOVAC_CC),-fno-tree-loop-distribute-patterns) \
            -nostdinc -isystem include -isystem $(NOVAC_BUILTIN_INC)
TGT_OBJ := $(patsubst %.c,$(BUILD)/target/%.o,$(LIB_SRC))

check-target:
	@$(NOVAC_CC) -dumpmachine | grep -q '^x86_64' || { echo "error: NOVAC_CC='$(NOVAC_CC)' does not target x86-64; refusing to build"; exit 1; }

lib: check-target $(BUILD)/target/libNovaC.a $(BUILD)/target/crt0.o
$(BUILD)/target/libNovaC.a: $(TGT_OBJ)
	rm -f $@
	$(NOVAC_AR) rcs $@ $^
$(BUILD)/target/%.o: %.c | check-target
	@mkdir -p $(dir $@)
	$(NOVAC_CC) $(TGT_FLAGS) -c $< -o $@
$(BUILD)/target/crt0.o: $(CRT_SRC) | check-target
	@mkdir -p $(dir $@)
	$(NOVAC_CC) $(TGT_FLAGS) -c $< -o $@

$(BUILD)/target/tests/target/%.o: tests/target/%.c | check-target
	@mkdir -p $(dir $@)
	$(NOVAC_CC) $(TGT_FLAGS) -c $< -o $@

# A static, libc-free executable: our crt0 + the program + libNovaC.a.
$(OUT): $(BUILD)/target/crt0.o $(BUILD)/target/tests/target/novac_target_test.o $(BUILD)/target/libNovaC.a
	@mkdir -p $(dir $@)
	$(NOVAC_LD) -m elf_x86_64 -static -nostdlib -z noexecstack -e _start -o $@ $^

# The program the target test execs, at the path it expects. NovarisOS
# stages it there in its initrd; on a host it is copied there for the run.
CHILD := $(BUILD)/target/novac_child.elf
$(CHILD): $(BUILD)/target/crt0.o $(BUILD)/target/tests/target/novac_child.o $(BUILD)/target/libNovaC.a
	@mkdir -p $(dir $@)
	$(NOVAC_LD) -m elf_x86_64 -static -nostdlib -z noexecstack -e _start -o $@ $^

child: $(CHILD)

target-test: $(OUT) $(CHILD)
	@file $(OUT) | grep -q "statically linked" || { echo "ERROR: $(OUT) is not static"; exit 1; }
	@cp $(CHILD) /tmp/novac_child
	@./$(OUT); rc=$$?; rm -f /tmp/novac_child; if [ $$rc -eq 131 ]; then echo "PASS: target test exited 131"; else echo "FAIL: exit $$rc"; exit 1; fi

check-sysnum:
	$(HOSTCC) -std=gnu11 -I../../include -fsyntax-only tests/target/check_sysnum.c
	@echo "PASS: syscall numbers match include/syscall64.h"

example: check-target
	@mkdir -p $(BUILD)/target/examples
	$(NOVAC_CC) $(TGT_FLAGS) -c examples/demo.c -o $(BUILD)/target/examples/demo.o

# ---- Host tests (portable core only; rename.h keeps symbols apart from libc) -
HOST_INC  := $(shell $(HOSTCC) -print-file-name=include)
SAN_FLAGS := -fsanitize=address,undefined
HOST_SAN  := $(shell $(HOSTCC) $(SAN_FLAGS) -x c -c /dev/null -o /dev/null 2>/dev/null && echo $(SAN_FLAGS))
HOST_OPT ?= -O1
HOST_COMMON := -std=c11 -g $(HOST_OPT) $(WARN) $(call cc-option,$(HOSTCC),-fno-tree-loop-distribute-patterns) \
               $(HOST_SAN) -fno-omit-frame-pointer
HOST_LIB_FLAGS := $(HOST_COMMON) -ffreestanding -fno-builtin -nostdinc -isystem include \
               -isystem $(HOST_INC) -include tests/host/rename.h
HOST_LIB_OBJ  := $(patsubst %.c,$(BUILD)/host/%.o,$(PORTABLE_SRC))
HOST_TEST_SRC := $(wildcard tests/host/test_*.c)
HOST_TEST_OBJ := $(patsubst %.c,$(BUILD)/host/%.o,$(HOST_TEST_SRC))

$(BUILD)/host/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_LIB_FLAGS) -c $< -o $@
$(BUILD)/host/tests/host/test_%.o: tests/host/test_%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_LIB_FLAGS) -Wno-format -c $< -o $@
$(BUILD)/host/tests/host/harness.o: tests/host/harness.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_COMMON) -c $< -o $@
$(BUILD)/host/tests/host/host_sys.o: tests/host/host_sys.c
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_COMMON) -c $< -o $@

$(BUILD)/host/run-tests: $(HOST_LIB_OBJ) $(HOST_TEST_OBJ) $(BUILD)/host/tests/host/harness.o $(BUILD)/host/tests/host/host_sys.o
	$(HOSTCC) $(HOST_SAN) $^ -o $@

host-test: $(BUILD)/host/run-tests
	./$(BUILD)/host/run-tests

clean:
	rm -rf $(BUILD)
