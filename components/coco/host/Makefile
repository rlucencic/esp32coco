# Host build of the Color Computer core: make -C components/coco/host
#   build/coco_host <2|3> [frames] [out.ppm]   (see host_main.c)
# The 6809 core is esp32emu's
COCO      := ..
M6809     := ../../../../esp32emu/components/m6809
EMUDBG    := ../../../../esp32emu/components/emudbg
DISKSPACE := ../../../../esp32emu/components/diskspace
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DM6809_HOST_HOOK \
             -I$(COCO)/include -I$(COCO) -I$(M6809)/include -I$(EMUDBG)/include -I$(DISKSPACE)/include

COCO_SRCS := $(wildcard $(COCO)/*.c) $(filter-out $(COCO)/host/%,$(wildcard $(COCO)/*/*.c))
COCO_OBJS := $(patsubst $(COCO)/%.c,$(BUILD)/coco/%.o,$(COCO_SRCS)) \
             $(BUILD)/m6809.o $(BUILD)/m6809_dis.o $(BUILD)/m6809_dbg.o $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o \
             $(BUILD)/diskspace_coco.o
HEADERS   := $(wildcard $(COCO)/*.h $(COCO)/include/*.h) $(M6809)/include/m6809.h $(EMUDBG)/include/emudbg.h \
             $(DISKSPACE)/include/diskspace.h

all: $(BUILD)/coco_host

$(BUILD)/coco_host: host_main.c $(COCO_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/dmk_test: dmk_test.c $(COCO_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

check-dmk: $(BUILD)/dmk_test
	python3 dmk_fixture.py $(BUILD)/fixture.dmk $(BUILD)/invalid.dmk
	cp $(BUILD)/fixture.dmk $(BUILD)/fixture-dmk.dsk
	./$(BUILD)/dmk_test $(BUILD)/fixture.dmk $(BUILD)/invalid.dmk $(BUILD)/fixture-dmk.dsk

# Mid-line palette writes (needs the ROMs: COCO_ROMS, default ../../../roms)
check-raster: $(BUILD)/coco_host
	@for mode in 640 512; do for first in 4 44; do \
	    python3 rastercart.py $(BUILD)/raster-$$mode-$$first.ccc --first $$first --mode $$mode && \
	    COCO_ROMS=$${COCO_ROMS:-../../../roms} COCO_CART=$(BUILD)/raster-$$mode-$$first.ccc \
	        ./$(BUILD)/coco_host 3 400 $(BUILD)/raster-$$mode-$$first.ppm > /dev/null && \
	    python3 rastercart.py expect $(BUILD)/raster-$$mode-$$first.ppm --first $$first --mode $$mode || exit 1; \
	done; \
	python3 rastercart.py $(BUILD)/stripes-$$mode.ccc --mode $$mode --stripes && \
	COCO_ROMS=$${COCO_ROMS:-../../../roms} COCO_CART=$(BUILD)/stripes-$$mode.ccc \
	    ./$(BUILD)/coco_host 3 400 $(BUILD)/stripes-$$mode.ppm > /dev/null && \
	python3 rastercart.py expect $(BUILD)/stripes-$$mode.ppm --mode $$mode --stripes || exit 1; \
	done

$(BUILD)/coco/%.o: $(COCO)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(M6809)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# (only the Color Computer's counters of esp32emu's diskspace)
$(BUILD)/diskspace_coco.o: $(DISKSPACE)/coco.c $(DISKSPACE)/include/diskspace.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all check-dmk check-raster clean
