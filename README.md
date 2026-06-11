# Looperoni

A live multitrack **loop-pedal takeover** for the [Ableton Move](https://www.ableton.com/move/),
built as a module for [Schwung](https://schwung.dev) ("Move Everything").

Think grid-style live looper (à la the popular iPad loop apps), reimagined for
Move's hardware: the 4×8 pad grid becomes a bank of **32 loop cells** that you
record, overdub, play and stop — all tempo-synced, hands-on, no screen-staring.

> Status: **early scaffold (v0.1).** The full module structure, UI and a working
> DSP loop engine are in place. It still needs to be cross-compiled and verified
> on real hardware — see [docs/PLAN.md](docs/PLAN.md).

## How it plays

| Action | Result |
| --- | --- |
| Press an **empty** pad | start recording (on the next bar in Bar mode) |
| Press a **recording** pad | commit the loop, start playing |
| Press a **playing** pad | stop it (audio kept) |
| Press a **stopped** pad | play it again |
| **Delete** + pad | clear that loop |
| **Record** button | overdub the selected loop |
| **Play** button | stop-all / play-all |
| **Loop** button | cycle quantize: Bar → Beat → Free |
| **Up / Down** | tempo ±1 BPM (Shift ±10) |
| **Jog** | select loop (Shift+Jog = tempo); click = overdub |
| **Knob 1 / 2 / Master** | loop volume / input monitor / master volume |
| Shift + Volume-touch + Jog-click | exit the takeover (handled by Schwung) |

The **first loop you record sets the cycle length** ("loop master"); everything
else snaps to whole cycles so layers stay phase-locked.

LED colours: red = recording, green = playing, amber/yellow = overdubbing,
dim green = stopped-with-audio, off = empty.

## Repository layout

```
module.json            Schwung module manifest (overtake + DSP)
ui.js                  Pad-grid UI: state, LEDs, display, DSP bridge
src/looperoni.c        DSP loop engine (records/overdubs/mixes audio)
src/host/              Vendored Schwung plugin ABI header
settings-schema.json   Schwung Manager settings
help.json              On-device help
release.json           Module-store release pointer
scripts/build.sh       Cross-compile dsp.so + stage dist/looperoni/
scripts/deploy.sh      scp a build straight onto the Move (dev loop)
scripts/Dockerfile     aarch64 build container
.github/workflows/     Tag -> build -> GitHub release
docs/ARCHITECTURE.md   How the pieces fit; on-device integration notes
docs/PLAN.md           Roadmap and what to do next
```

## Build & install

```bash
# Cross-compile for Move (aarch64). Needs an aarch64 gcc or Docker.
./scripts/build.sh
# -> dist/looperoni/{module.json,ui.js,dsp.so,help.json,settings-schema.json}

# Dev install straight onto a Move running Schwung:
MOVE_HOST=ableton@move.local ./scripts/deploy.sh

# Or release it: tag v0.1.0, let CI publish looperoni-module.tar.gz, then
# install via Schwung Manager (http://move.local:7700).
```

See [docs/PLAN.md](docs/PLAN.md) for the step-by-step bring-up plan and the
open hardware-verification questions.

## License

Released into the public domain (Unlicense) — see [LICENSE](LICENSE).

Not affiliated with or endorsed by Ableton. Schwung is a third-party,
unofficial project.
