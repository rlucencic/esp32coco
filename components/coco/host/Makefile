# Host build of the Color Computer core: make -C components/coco/host
#   build/coco_host <2|3> [frames] [out.ppm]   (see host_main.c)
# The 6809 core is esp32emu's
COCO      := ..
M6809     := ../../../../esp32emu/components/m6809
EMUDBG    := ../../../../esp32emu/components/emudbg
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DM6809_HOST_HOOK \
             -I$(COCO)/include -I$(COCO) -I$(M6809)/include -I$(EMUDBG)/include

COCO_SRCS := $(wildcard $(COCO)/*.c) $(filter-out $(COCO)/host/%,$(wildcard $(COCO)/*/*.c))
COCO_OBJS := $(patsubst $(COCO)/%.c,$(BUILD)/coco/%.o,$(COCO_SRCS)) \
             $(BUILD)/m6809.o $(BUILD)/m6809_dis.o $(BUILD)/m6809_dbg.o $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o
HEADERS   := $(wildcard $(COCO)/*.h $(COCO)/include/*.h) $(M6809)/include/m6809.h $(EMUDBG)/include/emudbg.h

all: $(BUILD)/coco_host

$(BUILD)/coco_host: host_main.c $(COCO_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/coco/%.o: $(COCO)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(M6809)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
