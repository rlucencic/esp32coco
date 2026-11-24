# esp32coco roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

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

6. **Match the 6809's interrupt and SYNC timing.** The [palette
   measurement](docs/gime-raster-2026-11-24.md) puts raster writes about
   three cycles (24–34 pixels at 1.79 MHz) earlier than XRoar. XRoar's 6809
   latches an interrupt line for a cycle and then spends two dead cycles
   leaving SYNC; `../esp32emu/components/m6809` resumes at once. In a
   scratch copy that adds the three cycles, XRoar and ours agree within
   about a cycle. Check the interrupt entry against the 6809 data sheet and
   XRoar, change the shared core, and build every project that uses it.
   Keep `check-raster` (with its expected positions moved), the replays,
   Disk BASIC's HALT/NMI byte loop and NitrOS-9 as controls.
