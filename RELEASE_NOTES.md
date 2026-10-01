# Kobra Slicer — Release Notes

## v1.0.0 — [date to be set at final build]

Kobra Slicer is an OrcaSlicer fork adding real, direct-from-slicer support for the Anycubic
Kobra 3 Max — upload and print over the printer's own MQTT connection, no intermediate app,
no Rinkhals/jailbreak required. Built and maintained independently by A to PC; not affiliated
with or endorsed by Anycubic.

### What's new

- **Real upload-and-print** for the Anycubic Kobra 3 Max, direct from the slicer — confirmed
  end-to-end against real hardware, not a theoretical integration.
- **ACE Pro multi-colour tool-changing** — confirmed via a real multi-colour print, correct
  tray-to-tray switching watched live on the physical printer.
- **Pre-print options** matching Anycubic's own Slicer Next "Start Print" dialog: bed
  levelling, resonance compensation, time-lapse, flow calibration.
- **"First colour override" selector** in the upload dialog (previously labelled plainly
  "Anycubic ACE Pro," which read as representing every tray in a multi-colour print — it only
  ever sets the first material's tray; later colour changes during printing are handled
  automatically from the sliced file itself, independent of this dialog), laid out as a
  horizontal row of trays rather than a stacked list for better readability with several ACE
  Pro trays.
- **Windows installer**, built with Inno Setup: correct per-machine install path, Start Menu
  and desktop shortcuts, a real uninstaller, Kobra Slicer's own branding throughout.

### Tested and verified

- Close to two full days of printing directly from Kobra Slicer to a real Kobra 3 Max, with
  zero issues traceable to Kobra Slicer itself. One upload stall was traced to the local
  network's DHCP server dropping a lease mid-session — a network fault, not a slicer or
  printer fault.
- Full machine and slicer calibration done from scratch: flow ratio 0.915 → 1.0, pressure
  advance 0.05 → 0.078, max volumetric speed 18 → 9.7mm³/s.
- Gcode output measured directly against Anycubic's own Slicer Next with byte-identical
  settings on the same model: Kobra Slicer's corner handling produces noticeably shorter
  segments immediately after sharp direction changes (0.44mm average vs. 1.20mm), independent
  of resolution/arc-fitting/wall-transition settings, which were confirmed identical in both
  outputs' own config dumps.
- A full torture-test print, honestly reported: bridging held, fine text stayed legible, and
  the model's most common community fail point (a thin spring/flower-trunk detail) stayed
  attached — but with a real layer-adhesion defect on that same feature. A mid-print filament
  runout (unplanned) recovered cleanly in roughly five minutes with no visible print defect.
- A targeted reprint — nozzle temp +10°C (220°C→230°C) and overhang speed reduced from
  30→20mm/s above 50° and 10→7mm/s above 75° — fixed the layer-adhesion defect and the 50°+
  overhang sagging (down to 1-2 layers at 60°, where it had been consistently worse). The
  steepest 75° overhang continued to sag somewhat even at the slowest speed tier tested.
  Retraction was also dropped 0.2→0.18mm in this round, aimed at stringing — direction was
  backwards for that goal (less retraction generally means *more* stringing, not less) and was
  corrected in the next round below.
- A second reprint — nozzle temp raised another 10°C (230°C→240°C initial layer) and
  retraction corrected to 0.22mm (up from the stock 0.2mm, the right direction to reduce
  stringing) — produced a further real, photographed improvement: clearer embossed text,
  cleaner bridging on the long arch, and the thin spring/flower-trunk detail (the original
  fail point) stayed attached and intact. Confirmed independently on a second, unrelated
  model (a smooth-curved-surface design from a different series) with flawless base text —
  the cleanest layer quality seen on this printer so far, though with one open, unexplained
  observation: a few small yellow dots on layer one, not seen on an earlier print from the
  same series. "Clean for what printed it" — good enough to stop chasing the torture-test
  model further and only revisit tuning if a real issue shows up on an actual project print.

### Known limitations

- **Only tested against a real Anycubic Kobra 3 Max.** Bambu, Elegoo, and the other
  manufacturer profiles shipped in this fork should work — every Anycubic-specific code path
  (gcode header, tool-change handling, pressure-advance override, tray-select dialog) was
  checked and confirmed properly gated so it cannot affect other brands — but none of them
  have actually been run against real hardware. Treat this as "should work, not yet verified,"
  not a tested claim.
- **75° overhangs still sag somewhat**, even after calibration and the speed/temperature
  tuning above. Use supports for very steep overhangs on this printer/filament combination —
  this looks like a real physical limit at that angle, not a bug to chase further.
- **Windows only.** macOS and Linux builds are not currently produced for this release.
- **No plain "Upload" action for the Kobra 3 Max** — only "Upload and Print" is offered.
  A plain upload (found by accident) left the printer's display stuck at "handshake"; Anycubic
  Slicer Next turns out to have no standalone upload action for this printer either, so there's
  no real firmware state for a plain upload to settle into. The button's been removed rather
  than left broken — this isn't a missing feature, it matches what Anycubic's own software
  actually supports.

### Known gaps, deferred to a later release

- OrcaSlicer's own calibration-generator functions (cornering, input shaping, VFA) write test
  overrides directly onto the live active print profile in memory rather than a disposable
  copy, confirmed by reading the source — upstream OrcaSlicer behaviour, not introduced by
  this fork. Whether this is unintended or a deliberate design choice (relying on the normal
  "unsaved changes" prompt as the safety net) hasn't been confirmed against any upstream
  issue tracker or maintainer statement. Not urgent either way for this release — calibration
  here was done safely by discarding the resulting prompt afterward. A disposable-copy
  approach is logged as a possible improvement for 1.1.0 regardless of which it turns out to be.
- Viewing Kobra LAN Monitor's live camera/status feed from inside Kobra Slicer itself (instead
  of needing the separate app open alongside the slicer) is planned, not yet built.

---

*How this got built: this release is the result of a human/AI collaboration — Jason (A to PC)
directing every real-world test, calibration run, and hardware decision, with Claude handling
the code changes, firmware/source investigation, and documentation. Every claim above is tied
to a real test run against physical hardware, not simulated or assumed.*
