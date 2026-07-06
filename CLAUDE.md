# Looperino — project instructions

Live-looper **overtake module** for [Schwung](https://github.com/charlesvestal/schwung)
on Ableton Move. Owner: Arvid (Somnitec).

**Read [docs/DESIGN.md](docs/DESIGN.md) before doing anything** — it is the
canonical spec: every locked decision from the scoping Q&A, the open
questions, and the milestone order. Don't re-litigate locked decisions;
open questions get decided on-device by feel, with Arvid.

## Context you'd otherwise have to rediscover

- [docs/platform-research.md](docs/platform-research.md) — sourced facts on
  Move hardware, Schwung internals, and the reference instruments (Loopy Pro
  loop lifecycle, OP-Z punch-in FX/step components/master track, loop artists'
  rigs). Trust it over memory, but it's point-in-time (2026-07-06).
- [docs/dev-notes.md](docs/dev-notes.md) — build/deploy/debug workflow.
- `reference/` (gitignored) — local clones for study:
  `schwung` (framework + docs/), `schwung-twinsampler` (closest existing
  overtake sampler), `magneto-move` (tape looper — live input handling),
  `Schwung-Module-Creator-skill` (distilled module-writing knowledge).
  If missing, re-clone from GitHub (same names, users: charlesvestal, jrucho,
  filliformes, xbraindance).

## Environment

- Dev machine (verified 2026-07-06): VS Code runs as a **snap with full host
  access** — Fedora host tools work directly, **rootless podman works** (used
  for cross-compiling; SELinux needs `:Z` on volume mounts). Older notes
  claiming a flatpak sandbox are stale.
- The Move is on the local network: SSH via `ableton@move.local` (key added
  through http://move.local/development/ssh). Schwung Manager web UI:
  http://move.local:7700. Modules live in `/data/UserData/schwung/modules/`.
- Device state: Move firmware 2.0.5, Schwung v0.11.4. Firmware updates
  disable Schwung until its installer is re-run.
- Target: 44.1 kHz / 128-frame stereo int16 blocks; DSP is cross-compiled
  C/C++ (see docs/dev-notes.md for the toolchain that actually works here);
  UI/logic is QuickJS ES2023 — no Node APIs.

## Working agreements

- Milestone order in DESIGN.md is deliberate: M0 (input/latency spike) gates
  everything — don't build looper features before it passes on hardware.
- On-device feel beats correctness-on-paper: anything interaction-related
  (gesture timings, LED language, FX ranges) ships as a tweakable constant.
- Realtime DSP rules (schwung docs/REALTIME_SAFETY.md): no allocation, no
  blocking, no unbounded loops on the audio path.
- Arvid wants to be consulted on genuine design forks — state the tradeoff in
  1–2 lines and let him choose. Don't use the interactive AskUserQuestion tool
  (it has frozen his VS Code); ask in plain text.
