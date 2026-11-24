/* Controller-level DMK checks. No guest ROMs are required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coco_internal.h"

enum {
    SELECT = 0xFF40, COMMAND = 0xFF48, TRACK = 0xFF49, SECTOR = 0xFF4A, DATA = 0xFF4B,
    BUSY = 0x01, DRQ = 0x02, CRC = 0x08, RNF = 0x10, DELETED = 0x20, PROTECTED = 0x40,
};

/* Host builds enable the CPU trace hook. The test drives the FDC directly. */
void m6809_host_hook(m6809_t *cpu)
{
    (void)cpu;
}

static uint8_t read_command(uint8_t command, uint8_t *data, size_t expected_len)
{
    size_t len = 0;
    fdc_write(COMMAND, command);
    uint8_t status = fdc_read(COMMAND);
    while (status & BUSY) {
        uint64_t event = fdc_next_event();
        assert(event != NEVER && event >= coco.cpu.cycles);
        coco.cpu.cycles = event;
        fdc_event();
        status = fdc_read(COMMAND);
        if (status & DRQ) {
            assert(len < expected_len);
            data[len++] = fdc_read(DATA);
            status &= ~DRQ;
        }
    }
    assert(len == expected_len);
    return status;
}

static void check_sector(uint8_t sector, unsigned length, uint8_t value, uint8_t status)
{
    uint8_t data[256];
    assert(length <= sizeof(data));
    fdc_write(SECTOR, sector);
    assert(read_command(0x88, data, length) == status);
    for (unsigned i = 0; i < length; i++) {
        assert(data[i] == value);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    fdc_reset();
    assert(!coco_disk_insert(0, argv[2]));
    assert(coco_disk_path(0) == NULL);
    /* the same DMK named .dsk: found by its header, read only */
    assert(coco_disk_insert(0, argv[3]));
    assert(coco_disk_write_protected(0));
    assert(fdc_disk_bytes(0, false) == 0);
    coco_disk_eject(0);
    assert(coco_disk_insert(0, argv[1]));
    assert(coco_disk_write_protected(0));
    fdc_write(SELECT, 0x29);  /* Drive 0, motor, double density. */

    uint8_t track[0x1900 - 128], reference[sizeof(track)];
    FILE *fixture = fopen(argv[1], "rb");
    assert(fixture && fseek(fixture, 16 + 128, SEEK_SET) == 0);
    assert(fread(reference, 1, sizeof(reference), fixture) == sizeof(reference));
    fclose(fixture);
    assert(read_command(0xE0, track, sizeof(track)) == 0);
    assert(memcmp(track, reference, sizeof(track)) == 0);

    const unsigned offsets[] = {160, 520, 900, 1200, 1260, 1600, 2000, 160};
    uint8_t id[6];
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++) {
        assert(read_command(0xC0, id, sizeof(id)) == 0);
        assert(memcmp(id, reference + offsets[i] - 128 + 1, sizeof(id)) == 0);
        assert(fdc_read(SECTOR) == 7);
    }

    fdc_write(TRACK, 7);
    check_sector(1, 256, 0x11, 0);
    check_sector(3, 128, 0x33, DELETED);
    check_sector(2, 128, 0x22, CRC);
    check_sector(9, 0, 0, RNF);
    check_sector(5, 0, 0, RNF);     /* no data mark after its ID */
    check_sector(6, 0, 0, RNF);     /* data mark past the 43-byte window */
    check_sector(8, 128, 0x88, 0);  /* size code 4: 128 bytes */

    /* Sector 4's data runs over sector 5's ID: read as it is, CRC good */
    uint8_t data[128];
    fdc_write(SECTOR, 4);
    assert(read_command(0x88, data, sizeof(data)) == 0);
    assert(memcmp(data, reference + 1224 - 128, sizeof(data)) == 0);
    assert(data[1257 - 1224] == 0xA1 && data[1260 - 1224] == 0xFE && data[1263 - 1224] == 5);

    /* Read Sector Multiple stops at the CRC error */
    fdc_write(SECTOR, 1);
    uint8_t multiple[256 + 128];
    assert(read_command(0x98, multiple, sizeof(multiple)) == CRC);
    assert(multiple[0] == 0x11 && multiple[255] == 0x11 && multiple[256] == 0x22);
    fdc_write(SECTOR, 1);
    assert(read_command(0x8A, NULL, 0) == RNF);  /* Compare against side 1. */
    fdc_write(TRACK, 0);
    assert(read_command(0x88, NULL, 0) == RNF);  /* Physical track 0 has ID 7. */

    fdc_write(COMMAND, 0xA0);
    assert(fdc_read(COMMAND) == PROTECTED);
    fdc_write(COMMAND, 0xF0);
    assert(fdc_read(COMMAND) == PROTECTED);
    assert(!coco_disks_dirty());
    coco_disk_eject(0);

    puts("DMK controller checks passed");
    return 0;
}
