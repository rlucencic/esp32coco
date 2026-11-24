# esp32coco roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

1. **Replay original software on both machine generations.** Add a CoCo 2
   PMODE/composite title and a CoCo 3 GIME title to `COCO_SCRIPT` replays in
   `components/coco/host`. Capture a playable checkpoint and PCM for each, then
   test physical sound and mouse/touch joystick control on the boards.

2. **Prototype DMK reads with a track fixture.** Start in
   `components/coco/fdc/fdc.c` with a read-only DMK track whose IDs are out of
   order and include a deleted-data mark. Check Read Address, Read Sector, Read
   Track and status against the fixture. Keep JVC `DSKINI` and save/reopen as
   controls.

3. **Quantify CoCo 3 raster changes.** Generate a mid-scanline palette or mode
   switch and capture its horizontal position against a reference. Use this to
   bound a change to the GIME renderer, with unchanged WIDTH 80 and HSCREEN 2
   output as controls.
