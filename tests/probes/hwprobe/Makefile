# hwprobe ROM. Needs the libdragon toolchain: N64_INST (see tests/probes/README.md).
ROMNAME := hwprobe
BUILD_DIR = build

ifeq ($(N64_INST),)
  $(error N64_INST is not set)
endif
include $(N64_INST)/include/n64.mk

# Build id: the first 12 hex digits of the SHA-1 over the sources, so a record names the exact
# source it came from (docs/timing/hwprobe.md, header).
BUILD_ID := $(shell cat $(sort $(wildcard *.c *.h *.S)) Makefile | shasum | cut -c1-12)
CFLAGS += -DHWP_BUILD_ID=\"$(BUILD_ID)\" -Wall -Werror -O2
# hwp_entry runs before libdragon's _start (entry.S).
LDFLAGS += -e hwp_entry

SRCS := main.c boot.c loops.c record.c flash.c pi.c display.c vectors.c probes.c probes2.c \
        entry.S exc.S loops_asm.S probes_asm.S probes2_asm.S rsp_probe.S
OBJS := $(addprefix $(BUILD_DIR)/,$(patsubst %.S,%.o,$(SRCS:.c=.o)))

all: $(ROMNAME).z64
.PHONY: all

$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)
$(ROMNAME).z64: N64_ROM_TITLE = "Ultra hwprobe"
$(ROMNAME).z64: N64_ROM_SAVETYPE = flashram
$(ROMNAME).z64: N64_ROM_REGIONFREE = true

clean:
	$(RM) -r $(BUILD_DIR) $(ROMNAME).z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)
