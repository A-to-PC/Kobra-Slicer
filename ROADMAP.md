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
