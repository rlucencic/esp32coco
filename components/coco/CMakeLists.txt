# Tandy Color Computer 2 and 3 (platform independent; host build in host/)
idf_component_register(SRCS "coco.c" "cpu/cpu.c" "memory/memory.c" "sam/sam.c" "pia/pia.c"
                            "vdg/vdg.c" "vdg/font6847.c" "gime/gime.c" "gime/gime_video.c" "gime/font8x8.c"
                            "keyboard/keyboard.c" "joystick/joystick.c" "sound/sound.c"
                            "cartridge/cartridge.c" "cassette/cassette.c" "fdc/fdc.c" "harddisk/vhd.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES m6809
                       PRIV_REQUIRES emudbg esp_psram)

# Runs in the emulation loop. Jump tables for the switches (the IDF turns
# them off for code that may run with the flash cache disabled; the
# emulation task never does: flash writes stall it)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
