# Kobra Slicer — Roadmap

Jason's own plan, logged as stated so it isn't lost between sessions.

## Real calibration day, 30/09/2026 — confirmed locked in

Full machine + slicer recalibration, done properly from scratch (not inherited from Slicer
Next's broken wizards or an untrusted download). Final values, confirmed actually saved in the
real profile (`user/default/filament/PLA.json`, checked directly, not assumed):

| Parameter | Was | Now |
|---|---|---|
| Flow ratio | 0.915 | **1.0** |
| Pressure advance | 0.05 | **0.078** |
| Max volumetric speed | 18 | **9.7** |
| Retraction | 0.2 | 0.2 (confirmed still correct, no stringing) |
| Nozzle temp | — | 220 first layer / 210 standard, bed 60 |

Cornering, input shaping and VFA cal prints: VFA ran clean (no banding at any tested
speed/angle, left at default — genuinely no change needed, not skipped). Cornering and input
shaping were left at default for 1.0.0, not run — see the real bug below.

## Real bug found and independently verified, 30/09/2026 — OrcaSlicer's own calibration generator

Not Anycubic-specific — this is upstream OrcaSlicer behaviour this fork inherited, so it would
affect any printer brand's cornering/input-shaping/VFA calibration prints, not just the K3M.
Confirmed directly by reading the actual source, not taken on anyone's word:
`Plater::calib_VFA()` (`Plater.cpp` ~13425, and the equivalent cornering/input-shaping
functions) takes a direct pointer to the **live, active** preset's config
(`preset_bundle->prints.get_edited_preset().config`) and calls `set_key_value()` straight onto
it — wall loops, infill, spiral mode, overhang speed, and more, all real settings, no temporary
copy made first. Running one of these calibration tests genuinely mutates your actual active
profile in memory; only noticing the resulting "unsaved changes" indicator and discarding it
stops that mutation from landing in your real saved profile.

**Fix, for 1.1.0** (not urgent for 1.0.0 — real prints are clean on the locked values above,
cornering/input-shaping just stay at sensible defaults until this lands): have the cal-print
functions clone the active preset, apply the test overrides to the clone, generate gcode from
the clone, then discard it — never touch the original. One shared fix covers cornering, input
shaping, and VFA, since they all go through the same pattern.

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
- [x] Gcode header (and `header_slic3r_generated()`, its non-Anycubic fallback) was writing the
      Anycubic-specific line unconditionally, for every printer brand — gated properly behind
      `is_anycubic_printer` 29/09/2026, restoring the original vanilla line for everyone else.
      Prompted directly by Jason's real intended use: one slicer for K3M, Elegoo, and Bambu.
- [ ] (add more here as they turn up — don't fix opportunistically mid-debugging)

Each item: change it, test it against the real printer, only then consider it closed. Once the
list is clear, that's the release build.

## Multi-colour printing — confirmed working, 29/09/2026

Real test: two objects, single simple test shape, each assigned a different ACE Pro tray
(1/yellow, 3/purple), Multi Colour process profile (prime tower enabled — the plain single-colour
profiles have it forced off, would have caused real colour bleed at tool changes; caught before
sending). Sent, printed, and watched live: correctly switched tray 1 → tray 3 → tray 1, at least
two real tool changes, both to the correct tray. Stopped intentionally at layer 8/25 once
confirmed — no need to print the whole test block.

Real gap found along the way, not a functional bug: the upload confirmation dialog's ACE Pro
tray selector (`PrintHostDialogs.cpp` ~2154) only ever represents the plate's *first* filament's
tray — real code comment confirms it was only ever built for single-tray confirmation, and
`m_selected_tray_index` really does feed `info["anycubic_tray_index"]` (confirmed by reading the
code, not assumed) — genuinely functional, just narrowly scoped and currently unlabeled as such.
This is why the `ams_box_mapping` sent at upload only had one entry. Doesn't affect whether
multi-colour actually prints correctly (it does — the real tray switching lives in the sliced
gcode itself, independent of this dialog).

**Agreed Phase 3 fix, 29/09/2026** (the low-effort, immediately valuable one — not the bigger
"represent every tray" idea, which stays a future nice-to-have): relabel the group box
**"First colour override"** rather than the current plain "Anycubic ACE Pro", plus a mouseover
tooltip explaining the scope plainly — something like *"Only affects the first material used —
later colour changes are handled automatically from the sliced file."* — so nobody mistakes it
for a full multi-colour tray confirmation.

Evidence: no distinct "tray changed" MQTT event field exists in the real capture to point to —
the switch itself was watched directly on the physical printer, not inferred from a log line.
Real primary evidence, same standing as several other confirmations this project has relied on.

## Release notes — multi-manufacturer support claim

Agreed wording, 29/09/2026: state multi-manufacturer support (Bambu, Elegoo, and every other
vendor whose profiles ship in this fork) as **should work, not tested** — reasoned through and
confirmed correct in code (every known Anycubic-specific change checked and confirmed properly
gated behind `is_anycubic_printer`/`m_is_anycubic_printers`: the gcode header, `source_info`,
the M900 pressure-advance override, tool-change/physical-extruder-id handling, the ACE Pro
tray-select dialog), but never actually run against real Elegoo or Bambu hardware — only a K3M
was available to test against. Be explicit about that gap in the release notes rather than
implying it's been verified.

## Phase 3 — Cosmetic and feature updates, post-release

Starts once Phase 2 is released and real-world feedback starts coming in (expected to move fast
once this is posted publicly, including to Anycubic's own K3M Facebook page).

- **Live monitoring via an inbuilt web view**: open Kobra LAN Monitor's own live camera/status
  feed inside Kobra Slicer itself (an embedded browser panel), as the main first post-release
  update — rather than needing a separate app running alongside the slicer. Real investigation
  done 29/09/2026 (while Jason was out), not yet built — deliberately, since it touches
  `MainFrame`'s tab system and needs real interactive testing, not a blind guess:
  - Hook point: the "Switch to Device tab after upload" checkbox already calls
    `mainframe->request_select_tab(MainFrame::TabPosition::tpMonitor)` in
    `PrintHost.cpp:371` — reuse this trigger, don't add a new checkbox.
  - **Don't insert a new tab.** `MainFrame`'s tab bar uses fixed integer positions
    (`tpProject=5` etc.) checked in dozens of places in `MainFrame.cpp` — inserting a page
    would shift every later tab's real runtime index while those hardcoded comparisons stay
    the same, a real risk of silently breaking other tab logic.
  - **Don't modify the existing Device tab's internals either.** It's `MonitorPanel`
    (`Monitor.hpp/cpp`, ~700 lines) wrapping `StatusPanel` (~6,000 lines) plus four more
    sub-panels — large, unfamiliar, Bambu-cloud-oriented. Not safe to edit blind.
  - **The safe path**: a small, standalone new panel class (same reasoning as
    `AnycubicAceTraySelectDialog` — deliberately not reusing the big generic Bambu dialog),
    just a `wxWebView` + fallback message, swapped in for `MonitorPanel` at the exact spot
    `m_monitor` gets constructed and added to the tab panel (`MainFrame.cpp` ~line 1327).
  - Reuse what already exists rather than building fresh: `WebView::CreateWebView(parent, url)`
    (`Widgets/WebView.cpp`, already used by `MarkdownTip`) for the embed itself, and the
    existing `WebView::CheckWebViewRuntime()` / `DownloadAndInstallWebViewRuntime()` pattern
    for the "WebView2 runtime missing" case — directly reusable for the "Kobra LAN Monitor not
    found, please download and install" fallback Jason wants, same shape of problem.
  - Real target URL: `http://localhost:8899` — confirmed from Kobra LAN Monitor's own
    `Program.cs` (`GetValue<int?>("HttpPort") ?? 8899`), the actual default a normal install
    uses. (Not `8900` — that was only today's manual override for a second, parallel capture
    instance during testing, not the real default.)
  - Detection: a short-timeout local HTTP probe to that URL before deciding whether to show
    the WebView or the "please install" message with a link.
- (add more here as they come in — expect requests to arrive quickly once this is public)
