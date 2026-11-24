# esp32coco roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

2. **Prototype DMK reads with a track fixture.** Start in
   `components/coco/fdc/fdc.c` with a read-only DMK track whose IDs are out of
   order and include a deleted-data mark. Check Read Address, Read Sector, Read
   Track and status against the fixture. Keep JVC `DSKINI` and save/reopen as
   controls.

3. **Quantify CoCo 3 raster changes.** Generate a mid-scanline palette or mode
   switch and capture its horizontal position against a reference. Use this to
   bound a change to the GIME renderer, with unchanged WIDTH 80 and HSCREEN 2
   output as controls.

4. **Check game audio and input by hand.** Use the [Tiler Tex and B.E.
   Break-In replays](docs/software-replay-2026-11-24.md). On the MaTouch,
   listen to both titles on the speaker and verify movement and fire
   from the panel and a physical mouse. Repeat the game checks on the Tab5.
