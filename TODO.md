# esp32coco roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

3. **Quantify CoCo 3 raster changes.** Generate a mid-scanline palette or mode
   switch and capture its horizontal position against a reference. Use this to
   bound a change to the GIME renderer, with unchanged WIDTH 80 and HSCREEN 2
   output as controls.

4. **Check game audio and input by hand.** Use the [Tiler Tex and B.E.
   Break-In replays](docs/software-replay-2026-11-24.md). On the MaTouch,
   listen to both titles on the speaker and verify movement and fire
   from the panel and a physical mouse. Repeat the game checks on the Tab5.

5. **Run protected CoCo DMK images.** The [DMK reads](docs/dmk-reads-2026-11-24.md)
   have only seen images made by `jvc2dmk.py` and the fixture. Find titles
   preserved as DMK because of their protection and run them on the host
   with `COCO_FDCLOG`. Add rotation timing (Read Address in the order the IDs
   pass the head, the index pulse against the raw track) or single density
   only where a loader needs it. Keep both replays' DMK and JVC runs
   identical as controls.
