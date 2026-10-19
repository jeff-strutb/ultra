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

SRCS := main.c boot.c loops.c record.c flash.c pi.c display.c vectors.c probes.c probes2.c sink.c cpu_func.c \
        rdp_render.c \
        entry.S exc.S loops_asm.S probes_asm.S probes2_asm.S cpu_func_asm.S rsp_probe.S
OBJS := $(addprefix $(BUILD_DIR)/,$(patsubst %.S,%.o,$(SRCS:.c=.o)))

# hwprobe-cold.z64: the same program behind libdragon's IPL3 patched to capture the CPU and
# RCP state at its first instruction (ipl3/cold-capture.patch). No retail CIC accepts that
# IPL3: boot it with SC64 direct boot (`sc64deployer upload --direct`), whose CIC emulation
# takes the checksum of the ROM's own IPL3.
LIBDRAGON_SRC ?= $(N64_INST)/../libdragon

all: $(ROMNAME).z64 $(ROMNAME)-cold.z64
.PHONY: all

ipl3/ipl3_cold.z64: ipl3/cold-capture.patch
	rm -rf $(BUILD_DIR)/ipl3
	mkdir -p $(BUILD_DIR)
	cp -R $(LIBDRAGON_SRC)/boot $(BUILD_DIR)/ipl3
	rm -rf $(BUILD_DIR)/ipl3/build $(BUILD_DIR)/ipl3/bin/*.z64
	cd $(BUILD_DIR)/ipl3 && patch -s -p1 < ../../ipl3/cold-capture.patch
	$(MAKE) -C $(BUILD_DIR)/ipl3 PROD=1 N64_INST=$(N64_INST) >/dev/null
	cp $(BUILD_DIR)/ipl3/bin/ipl3_prod.z64 $@

$(BUILD_DIR)/$(ROMNAME)-cold.elf: $(BUILD_DIR)/$(ROMNAME).elf
	cp $< $@
$(ROMNAME)-cold.z64: ipl3/ipl3_cold.z64
$(ROMNAME)-cold.z64: N64_ROM_HEADER = ipl3/ipl3_cold.z64
$(ROMNAME)-cold.z64: N64_ROM_TITLE = "Ultra hwprobe cold"
$(ROMNAME)-cold.z64: N64_ROM_SAVETYPE = flashram
$(ROMNAME)-cold.z64: N64_ROM_REGIONFREE = true

$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)
$(ROMNAME).z64: N64_ROM_TITLE = "Ultra hwprobe"
$(ROMNAME).z64: N64_ROM_SAVETYPE = flashram
$(ROMNAME).z64: N64_ROM_REGIONFREE = true

clean:
	$(RM) -r $(BUILD_DIR) $(ROMNAME).z64 $(ROMNAME)-cold.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)
