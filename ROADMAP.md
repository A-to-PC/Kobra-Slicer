# Kobra Slicer — Roadmap

Jason's own plan, logged as stated so it isn't lost between sessions.

## Phase 1 — Get it working (done, 29/09/2026)

Full upload-and-print working end to end against a real Kobra 3 Max, confirmed by a real
completed print. See `ANYCUBIC_INTEGRATION_NOTES.md` for every bug found and fixed to get here.

## Phase 2 — Minimal necessary cleanup, test, release

Go back through what was changed to get Phase 1 working and fix only what's actually
unnecessary or wrong — not a rewrite, not scope creep, just closing the gap between "works" and
"doesn't do anything it doesn't need to." Tracked as a checklist in
`ANYCUBIC_INTEGRATION_NOTES.md` under "Before release — deliberate workarounds to review".

- [x] Gcode header identity spoof (`AnycubicSlicerNext` → `Kobra Slicer`, confirmed the firmware
      only checks the literal prefix) — changed 29/09/2026, confirmed working via a real second
      test print same day (firmware accepted it, printed normally; see the proof folder).
- [x] Kobra Slicer's own version number — was borrowing `SLIC3R_VERSION` (OrcaSlicer's internal
      engine build number, `02.06.00.51`), which reads as a raw build string and, worse, implies
      Kobra Slicer tracks Orca's own releases, which won't stay true. Agreed with Jason
      29/09/2026: a new, independent `KOBRA_SLICER_VERSION` constant, real semantic versioning,
      decoupled from Orca's numbering entirely. **`0.1.0` now** (Phase 2, still testing),
      **`1.0.0` at first public release**, patch/minor bumps from there as updates land.
      `SLIC3R_VERSION` itself left untouched — it's relied on elsewhere for real 3mf/config
      compatibility checks, not safe to repurpose for this.
- [ ] (add more here as they turn up — don't fix opportunistically mid-debugging)

Each item: change it, test it against the real printer, only then consider it closed. Once the
list is clear, that's the release build.

## Phase 3 — Cosmetic and feature updates, post-release

Starts once Phase 2 is released and real-world feedback starts coming in (expected to move fast
once this is posted publicly, including to Anycubic's own K3M Facebook page).

- **Live monitoring via an inbuilt web view**: open Kobra LAN Monitor's own live camera/status
  feed inside Kobra Slicer itself (an embedded browser panel), as the main first post-release
  update — rather than needing a separate app running alongside the slicer.
- (add more here as they come in — expect requests to arrive quickly once this is public)
