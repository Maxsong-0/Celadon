Celadon is a personal plugin, shared privately and not sold. Once you have the installer for your platform, install it as described below, then **rescan plug-ins** in your host.

## macOS

- System: macOS 12 Monterey or later.
- Pick the installer for your chip: Apple menu → About This Mac. If the Chip line says Apple M…, use `Celadon-<version>-macOS-AppleSilicon.pkg`; if it lists an Intel Processor, use `Celadon-<version>-macOS-Intel.pkg`.
- Double-click the .pkg; under Customize you can tick only the formats you need:

| Format | Installed to |
|---|---|
| AU `Celadon.component` | `/Library/Audio/Plug-Ins/Components/` |
| VST3 `Celadon.vst3` | `/Library/Audio/Plug-Ins/VST3/` |
| Standalone `Celadon.app` | `/Applications/` (for trying the interface without a host) |

- AU identity: `aufx Cldn Maxs` (Max Song: Celadon). The AU passes `auval` on both chips.
- A host opened through Rosetta on Apple silicon is an x86_64 process and can only load the plug-ins from the Intel installer; in that case install the Intel package (macOS will offer to install Rosetta).
- If the .pkg says it's from an unidentified developer the first time (there's no Apple Developer ID signature), go to System Settings → Privacy & Security, find the Celadon entry and click Open Anyway.
- If you've built it yourself in Xcode and it lives in your personal folder `~/Library/Audio/Plug-Ins/`, don't run the installer as well, or your host will show two copies.

## Windows

- System: Windows 10 / 11, 64-bit.
- The installer `Celadon-<version>-Windows-x64-Setup.exe` puts the VST3 at `C:\Program Files\Common Files\VST3\Celadon.vst3` (fixed) and the standalone app (optional, installed by default) in `C:\Program Files\Celadon\`, with a Start menu shortcut; uninstall it from Settings → Apps. The wizard appears in Chinese or English depending on your Windows display language.
- Static runtime: no separate VC++ redistributable is needed.

> ⚠ The installer isn't code-signed. The first time you run it, SmartScreen warns about an unknown publisher: click More info → Run anyway.

## Hosts

- Tested: FL Studio 2026 and Ableton Live 12 Suite. Any other host that supports VST3 / AU should work in principle.
- There's no AAX (Pro Tools) version.
- After an update, a host may still have the old binary cached: save your project, then restart the host or rescan the plug-in folder.
- Sidechain: Learn reference under Target spectrum only reads the plugin's optional stereo sidechain input, “Reference”; route the reference track to the DAW's external sidechain (each host names it differently).

## Latency

Your host compensates it automatically. Total latency = BALANCE 0 + CLAY (linear-phase Mono D + peak-guard lookahead G) + COLOR 64 + LIMIT (Clip oversampling 60 + Lookahead + 62).

![Latency breakdown at 48 kHz with 2 ms Lookahead](figure:latency)

| Sample rate | Total latency (Lookahead 2 ms) |
|---|---|
| 44.1 kHz | 1287 samples ≈ 29.2 ms |
| 48 kHz | 1384 samples ≈ 28.8 ms |
| 96 kHz | 2584 samples ≈ 26.9 ms |
| 192 kHz | 4984 samples ≈ 26.0 ms |

When you change Lookahead, the new lookahead takes effect — and the new latency is reported — only once the parameter has been still for 0.3 s; latency doesn't change while you drag or automate it. Each module's bypass keeps that module's latency, so switching never misaligns anything.

## Old projects

- “Loud, not harsh”, Target spectrum, codec margin and dither, added in 0.1.8.5, all load as off in old projects, so they sound exactly as before; only new instances default to Auto.
- If an old project has no saved GLAZE value, it restores to 0 (strictly manual); an explicitly saved 0 stays 0.
- Genre and Delivery, added in 0.1.8.8, load as General and Standard in old projects, and the saved plan and calibration stay in effect.
- Window scale: `uiScale` from v0.1.7 and earlier is no longer read; old projects open at the default size set in Settings.

## Settings file (follows you, not the project)

Language, Reduce motion and the default size for new windows live in `Celadon.settings`: `~/Library/Application Support/Celadon/` on macOS, `%APPDATA%\Celadon\` on Windows. All instances in the same process share it — switch the language in one window and the others switch too.

## Performance

Budget use on a single audio thread (not total CPU): on an Apple M2 Pro with GLAZE 100 and the interface closed, about 1.3% at 48 kHz, 2.6% at 96 kHz and 5% at 192 kHz. At 192 kHz with a 32–64-sample buffer, older Intel laptops risk dropouts; don't raise the x86_64 instruction-set baseline (Rosetta has no AVX). Where the numbers come from: [Performance and verification](/algorithms/verification).
