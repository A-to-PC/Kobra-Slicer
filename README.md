<div align="center">

<picture>
  <img alt="Kobra Slicer logo" src="resources/images/KobraSlicer.png" width="15%" height="15%">
</picture>

[![GitHub Repo stars](https://img.shields.io/github/stars/A-to-PC/Kobra-Slicer)](https://github.com/A-to-PC/Kobra-Slicer/stargazers)

**Kobra Slicer** is an independent, unofficial fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer), maintained by A to PC, built to add real, working upload-and-print support for the Anycubic Kobra 3 Max — over the printer's real local LAN protocol, on completely stock firmware, no Rinkhals or jailbreak required.

Everything OrcaSlicer already does — ultra-fast slicing, intelligent support generation, advanced calibration tools — stays intact. This fork adds a real, dedicated Anycubic print host and Kobra 3 Max profiles on top of it, rather than replacing anything.

<h3>

# About this fork

</h3>

</div>

Anycubic's own Kobra 3 Max ships with Anycubic Slicer Next, and stock firmware otherwise has no working path to print from a current OrcaSlicer build — this project exists to close that gap. The real story of how it got built, bug by bug, is documented day-by-day in the [Kobra 3 Max Journey](https://github.com/A-to-PC/Kobra-3-Max-Journey).

# Main features

Inherited from OrcaSlicer, unchanged:

- **Advanced Calibration Tools** — temperature towers, flow rate, retraction & more.
- **Precise Wall and Seam Control** — adjustable outer wall spacing, scarf seams.
- **Sandwich Mode and Polyholes Support** — varied infill patterns, accurate hole shapes.
- **Overhang and Support Optimization** — printable overhangs, precise support placement.
- **Granular Controls and Customization** — fine-tuned speed, layer height, pressure, temperature.
- **Network Printer Support** — Klipper, PrusaLink, OctoPrint.
- **Mouse Ear Brims & Adaptive Bed Mesh**.
- **Wide Printer Compatibility** — Bambu Lab, Prusa, Creality, Voron, and more; every profile that ships with upstream OrcaSlicer is still here.

Added by this fork:

- **A real Anycubic Kobra 3 Max print host** — upload-and-print over the printer's own local MQTT protocol, built from genuine captured traffic, not guesswork.
- **Anycubic Kobra 3 Max / Kobra S1 Max / Kobra X profiles**, including real ACE Pro tray selection.
- Multi-manufacturer support (Bambu, Elegoo, and every other vendor's profiles) is expected to keep working unchanged — every Anycubic-specific addition is gated in code to only affect Anycubic printers — but is not yet tested against real hardware beyond the Kobra 3 Max, since that's the only printer this project has to test with.

# Wiki

Most slicer settings and general usage are unchanged from upstream, so [OrcaSlicer's own wiki](https://www.orcaslicer.com/wiki) is still a genuinely useful reference for those. This fork doesn't have a separate wiki of its own yet.

# Download

📥 **[Download the Latest Release](https://github.com/A-to-PC/Kobra-Slicer/releases/latest)**

Windows installer, from the [releases page](https://github.com/A-to-PC/Kobra-Slicer/releases).

# How to install

## Windows

Download and run the installer from the [releases page](https://github.com/A-to-PC/Kobra-Slicer/releases/latest).

- *If you have trouble running the build, you might need to install the following runtimes:*
    <details>
    <summary>Troubleshooting</summary>

  - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://go.microsoft.com/fwlink/p/?LinkId=2124703) — required for the in-app web views.
  - [vcredist2019_x64](https://aka.ms/vs/17/release/vc_redist.x64.exe) — this may already be installed if you've had Visual Studio on this machine before. Check `%VCINSTALLDIR%Redist\MSVC\v142` if unsure.
    </details>

Mac and Linux builds aren't currently produced for this fork — Windows is the only platform this project builds and tests against.

# How to Compile

This fork's own build script: `run_slicer_build.bat`, at the repo root. General OrcaSlicer build instructions (dependencies, toolchain setup) at the [OrcaSlicer Wiki — How to build](https://www.orcaslicer.com/wiki/how_to_build) page still apply, since the underlying build system is unchanged from upstream.

# Klipper Note

If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.

```gcode
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Supporting this project

This fork doesn't run its own separate sponsorship — if you'd like to support the underlying engine this is built on, OrcaSlicer's own sponsors are listed on [their repository](https://github.com/OrcaSlicer/OrcaSlicer). To report an issue or contribute to Kobra Slicer specifically, use [this repository's own issues](https://github.com/A-to-PC/Kobra-Slicer/issues).

## Some Background

Open-source slicing has always been built on a tradition of collaboration and attribution. [Slic3r](https://github.com/Slic3r/Slic3r), created by Alessandro Ranellucci and the RepRap community, laid the foundation. [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research built on Slic3r and acknowledged that heritage. [Bambu Studio](https://github.com/bambulab/BambuStudio) in turn forked from PrusaSlicer, and [SuperSlicer](https://github.com/supermerill/SuperSlicer) by @supermerill extended PrusaSlicer with community-driven enhancements. Each project carried the work of its predecessors forward, crediting those who came before.

[OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) began in that same spirit, drawing from BambuStudio, PrusaSlicer, and ideas inspired by CuraSlicer and SuperSlicer, and has since grown into the most widely used and actively developed open-source slicer in the 3D printing community.

Kobra Slicer continues that same tradition one step further: a fork of OrcaSlicer, built specifically to give the Anycubic Kobra 3 Max the real, direct print workflow its own stock firmware doesn't otherwise offer.

The original OrcaSlicer logo was designed by community member [Justin Levine](https://github.com/jal-co). Kobra Slicer's own logo and icon set were built for this fork specifically.

# License

- **Kobra Slicer**, like OrcaSlicer, is licensed under the GNU Affero General Public License, version 3.
- The **GNU Affero General Public License**, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.
- Includes a **pressure advance calibration pattern test** adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.
- The **Bambu networking plugin** is based on non-free libraries from BambuLab. It is optional and provides extended functionality for Bambu Lab printer users.
