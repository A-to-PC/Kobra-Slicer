# Anycubic Integration — Real Bugs Found and Fixed

Working notes for maintaining the Anycubic (Kobra 3 Max / K3M and family) print-host
integration: Upload and Print over the printer's real local MQTT protocol, plus the
gcode-output differences the K3M's own firmware actually requires.

Every entry below was found and confirmed against **real evidence** — either a genuine
MQTT capture of Anycubic's own Slicer Next talking to a real printer, a byte-for-byte
diff of a real Slicer Next `.gcode.3mf` export against this fork's own, or (for the
final and most serious bug) a live SSH session on the printer watching the actual
firmware process crash in real time. None of this was guessed from documentation —
Anycubic doesn't publish the protocol.

If Anycubic's firmware or Slicer Next changes in a future update, re-verify against a
fresh real capture/export rather than assuming these values still hold.

---

## The root cause: a single gcode comment line crashes the firmware

**File:** `src/libslic3r/GCode.cpp`, `GCode::append_full_config()` — `banned_keys`.

The K3M's real firmware (`gklib`, Anycubic's own Go-based Klipper) has a genuine,
unrecoverable bug: it cannot parse the `; filament_colour_type = ...` line that newer
OrcaSlicer versions write into every gcode's config-dump footer. Parsing it triggers a
Go slice-bounds panic, which kills the firmware process outright — no error message,
no log line, total silence, requiring a full power cycle to recover. This is the exact
mechanism behind the generic, misleading `"k3c is shutdowing, please try later"` /
error code `10111` rejection.

Confirmed independently by the Rinkhals project (a real, separate open-source K3M
firmware project) — their own `mmu_ace_metadata.py` documents this exact line and
error code as a known firmware bug and strips it from every file it touches:

```python
# Lines emitted by newer OrcaSlicer versions that Anycubic's Go firmware
# (gklib) cannot parse — causes slice-bounds panic (error 10111).
GCODE_STRIP_PREFIXES = ("; filament_colour_type",)
```

Confirmed directly against real files: a genuine Slicer Next export has **zero**
occurrences of this line anywhere; every export this fork produced before this fix had
it, once, in the full config dump.

**Fix:** added `"filament_colour_type"` to the existing `banned_keys` set that
`append_full_config()` already used to exclude sensitive keys (credentials, host URLs)
from the gcode. No new mechanism — one more entry in a list that already existed.

If a future OrcaSlicer upstream merge reintroduces other new config keys, treat any
of them as suspect until confirmed safe against a real capture — this firmware does
not fail gracefully on a key it can't parse.

---

## MQTT protocol — Upload and Print

Real topics, real payload shapes, and real sequencing were all reverse-engineered by
capturing genuine MQTT traffic between Slicer Next and a real printer (via a
passively-subscribing third client), not from documentation.

### Connection

- `src/slic3r/Utils/AnycubicMqtt.cpp` — hand-rolled minimal MQTT v3.1.1 client over
  this fork's existing Boost.Asio + OpenSSL, deliberately not a full MQTT library
  (only QoS-0 publish/subscribe is ever needed here).
- Credential discovery: `/info` (port 18910) + a signed `/ctrl` request, AES-128-CBC
  decrypted — the same real handshake Kobra LAN Monitor already does in C#, ported.
- `AnycubicLink::upload()` keeps **one persistent connection** open across the whole
  upload → verify → print/start → confirmation-burst sequence, handed off from the
  print-host dialog (`AnycubicPrintHostSendDialog`) via `attach_session()`. A second,
  separate connect/disconnect cycle was suspected (though never proven) of
  contributing to firmware-side confusion — kept as one connection regardless, since
  it matches the real client's own behaviour.

### Real, confirmed message sequence (single-material print, first slot)

1. `lastWill/query` — the *only* thing sent immediately before `print/start`. Nothing
   else. (`AnycubicMqttSession::send_pre_print_check()`)
2. `print/start`, on the **`slicer/printer/...`** topic (not `web/printer/...`).
3. Real client waits for the actual `print/report` response (`state`/`code`/`msg`) —
   this used to be pure fire-and-forget; not checking it is why the real rejection
   reason was invisible to users for so long.
4. **Only after real acceptance** (`state` other than `"failed"`): the full
   confirmation/telemetry burst — `print/query`, `calibration/getInfo`,
   `properties/read` (exact key list matters — see `AnycubicMqtt.cpp`),
   `extfilbox/getInfo`, `extrudeControl/getInfo`, `buried/PrintStart` (real
   analytics event, every field confirmed against a real capture — `cn_code` is a
   fixed, non-per-device constant), `info/net`, `print/getSliceParam`.
5. File-details verification (`file/fileDetails`) also happens **after** acceptance,
   as confirmation — not before, and not as a gate. Real Slicer Next never checks it
   first; moving it there was itself a real, confirmed bug (see below).

### Real payload fields, and the two most-confused ones

- **`paint_index` vs `ams_index` — not the same thing.** `paint_index` is the file's
  own colour, indexed by its position in the file's *own* used-filament list (always
  `0` for a single-material print, regardless of which project slot was used — the
  K3M has exactly one physical head). `ams_index` is the ACE Pro's own, separate
  concern: which physical tray was chosen. Confirmed by capturing the identical file
  printed to two different physical trays — `paint_index` never changed, `ams_index`
  did. Conflating the two (using the project's raw filament-slot number for
  `paint_index`) was a real, confirmed bug, duplicated in two places in
  `GCode.cpp`'s header-writing code.
- **`filesize` vs `gcode_size`.** `filesize` in `print/start` is the real, compressed
  `.gcode.3mf` archive's own disk size. `gcode_size` in `buried/PrintStart` is the
  *decompressed* internal `plate_N.gcode` text size (confirmed ~64x larger on the
  same real file). Don't conflate them.
- **Pressure advance is `M900 K<value>`, not the native Klipper macro**, on this
  firmware specifically — even though `gcode_flavor: klipper` is set identically in
  both this fork's and the real profile. Confirmed: the real firmware keeps the older
  syntax for this one command regardless of the declared flavour elsewhere. See
  `GCodeWriter.cpp::set_pressure_advance()`.

---

## Gcode-file-content bugs (found by full-file diff against real exports)

- **Missing `Metadata/plate_N.gcode.metadata`** — a lightweight header-only sibling
  file (thumbnail/config/AMS info, no toolpath) the firmware reads without loading
  the whole multi-megabyte file. Generated at export time now.
- **`M75 ; The first extruder is ready.`** — a required command, never written at
  all (gated to BambuLab printers only in the original code). Enabled for Anycubic.
  Very likely the literal cause of the K3M's own `CODE: 10133` "missing required
  commands" on-device error before this was found.
- **`; EXECUTABLE_BLOCK_HEAD`** marker — same story, silently never written.
- **Machine-limit commands** (`M201`/`M203`/`M204`/`M205`) — excluded for *any*
  Klipper-flavour printer on the (generally reasonable) assumption that real Klipper
  takes limits from `printer.cfg`, not gcode. The K3M's own firmware disagrees —
  confirmed the real file writes this block anyway. Enabled specifically for
  Anycubic, Klipper assumption left intact elsewhere.
- **Second gcode thumbnail** (512×512) — `thumbnails_internal` /
  `thumbnails_internal_switch` existed as dead JSON in the machine profile, never
  registered as real `ConfigOptionDef` entries anywhere in the C++ (confirmed by
  grep — zero hits) *and* never added to the separate, hardcoded printer-options
  allowlist `Preset::printer_options()` uses to validate loaded machine profiles
  (`Preset.cpp`) — the second bug caused a real, confirmed crash (`opt_bool()` on a
  silently-stripped, nonexistent option dereferences a null pointer). Both fixed.
- **Header `source_info` / `paint_info` / `project_info`** — missing from the header
  entirely; present later in a separate `ams_info` block only. Both are now written,
  using `print.extruders()` (the pre-computed, print-level list) rather than
  `m_writer.extruders()`, which is only populated later in the export pipeline and
  was empty at the point this header code runs.
- **`flush_volumes_chan_multipliers`** — a real, distinct per-channel config key
  (confirmed via a real `project_settings.config` diff), was being approximated by
  reusing the unrelated `flush_multiplier` key. Registered properly; needed adding
  to the `STATIC_PRINT_CONFIG_CACHE` struct in `PrintConfig.hpp`, not just as a
  `PrintConfigDef` entry, since `GCode.cpp` accesses it via direct member access.
- **Filename format** — the Kobra 3 Max process profiles (0.4/0.6/0.8 nozzle, every
  layer-height variant) used a plain `{input_filename_base}_..._{print_time}.gcode`
  template with no timestamp or plate number, while this fork's own Kobra S1 Max and
  Kobra X profiles already carried the correct `{timestamp}-...plate(NN)...` format
  matching real Slicer Next output. Applied the already-correct sibling template.
- **`filament_id`** — every Anycubic PLA variant across every printer profile shared
  the same generic placeholder (`GFL99`). Real Slicer Next system profiles each carry
  their own distinct real ID (`GFPLA`, `GFPLA Matte`, `GFPLA Silk`, `GFPLA+`, `GFPLA
  High Speed`). Fixed for every profile with a real reference value.
- **`X-BBL-Client-Type` / `X-BBL-Client-Version`** — an un-rebranded Bambu Lab
  identifier left in `slice_info.config`'s own header block (a different file from
  the gcode header's own producer string, missed by an earlier rename pass). Real
  file uses `X-ACNext-Client-Type` / `X-ACNext-Client-Version`, exactly two items.
- **`Metadata/filament_sequence.json`** — a real OrcaSlicer feature (filament
  swap-order tracking) Anycubic's own fork doesn't use; present in this fork's
  export, absent from the real one. Disabled for export, left in source as a real,
  working feature for non-Anycubic printers.
- **`; print_time` formatting** — gcode's own embedded metadata wrote raw seconds
  (`4455.78`); real format is human-readable (`1h 14m 15s`), same formatter the
  gcode's own estimated-time lines already use elsewhere.
- **Tool-select number** — was using the raw AMS/tray index for the gcode's own
  `T`-command. The K3M has one physical nozzle; tray switching is the ACE Pro's own,
  separate concern (`ams_box_mapping` handles it over MQTT). Fixed to use the
  already-correctly-resolved physical extruder id specifically for Anycubic, leaving
  Bambu's genuine multi-nozzle printers (where the raw id *is* the physical id)
  unchanged.

---

## Known-inert profile content — do not "fix" without new real evidence

`change_filament_gcode` in the Anycubic Kobra 3 Max machine profile contains a large,
entirely `;;;`-commented-out Bambu-AMS-style wipe-tower purge block. This looks like
dead, unported cruft — it isn't. Confirmed via the real file's *own* config dump: real
Slicer Next ships the byte-identical disabled block. It's genuinely inert firmware-side
dead code in both projects, not a bug. Was mistakenly "fixed" once this session and
reverted after checking the real file directly — a reminder to verify against the
actual config dump, not just the executable gcode body, before concluding a profile
value is wrong.

---

## Before release — deliberate workarounds to review

Things intentionally hardcoded to get a working end-to-end print, flagged by Jason
29/09/2026 for a proper pass once base functionality (this whole document) is
confirmed solid: get it working first, then go back and check what was changed that
may not actually be necessary, fix/reconsider those, and only then call it a release
build. Add to this list as more turn up; don't fix any of them opportunistically
mid-debugging — that's how the last multi-day investigation kept losing the thread.

- **The gcode header spoof** (`src/libslic3r/GCode.cpp` ~2578-2661): every gcode file
  Kobra Slicer writes claims `; generated by AnycubicSlicerNext 2.4.2` and an embedded
  `source_info.software_version` of the same, because the K3M's real firmware upload
  handler rejects anything whose header doesn't start with the literal prefix
  `AnycubicSlicer` — confirmed against the real extracted firmware binary. This was a
  deliberate spoof to get the printer to accept the file at all, not an accident. Worth
  checking before release whether the firmware only actually cares about the prefix (in
  which case the version number and app name after it could say something honest, e.g.
  `AnycubicSlicer / Kobra Slicer 2.6.0`) or whether more of the literal string is
  load-bearing — untested, don't assume either way without checking the real firmware
  behaviour first.

---

## Known bugs in Rinkhals' own components (not this project's — for reference only)

Found while tracing the root-cause crash above, via a live SSH session with Rinkhals
temporarily installed (removed again afterward — a full stock firmware reflash,
verified against the real update script, wipes it completely including SSH access).

- **`mmu_ace.py`'s `_toggle_tools_in_gcode()`** opens the *same* gcode file the
  firmware is concurrently loading, in `r+b` mode, and does an in-place binary
  seek-and-write with no locking against the firmware's own read. A real, plausible
  race condition if the ACE Pro's connection state changes mid-print-start. Not
  relevant to stock firmware (this component doesn't exist without Rinkhals) — noted
  here only because it was found during this investigation and is worth reporting
  upstream to that project separately.
