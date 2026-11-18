# Changelog

## 0.2.2 — 2026-11-18

First public release.

- **Natural mode:** your voice is posted to the game's StimulusManager as a movement-noise stimulus (the same kind
  footsteps and the motion tracker use), every 250 ms while you speak. Distance along the walkable path, vents,
  sense sets, meter accumulation and the behaviour trees are all handled by the game.
- Loudness mapped onto the game's own scale (footsteps 0.2–0.6, motion tracker 0.75–1.0), 6 dB per doubling.
- Positional uncertainty when the Alien is far or in a vent: the noise comes from a spot on your recent trail,
  never closer to the Alien than you are, and it tightens as the Alien approaches.
- Microphone: room-noise calibration, adaptive floor, digital-silence (mute) detection with automatic
  recalibration, guard against continuous noise (`MaxUtteranceMs`).
- Hook on the call to `StimulusManager::Tick` (runs only while the AI simulates); every game address byte-checked
  at startup, fail-closed.
- Forced mode kept as a fallback (`Modo=forcado`): graded NPC_FakeSense events with distance/vent-aware certainty.
- Test hotkeys (F7/F8), English log messages, Linux and Windows installers.

## 0.1 — 2026-11-17 (unreleased)

- Prototype: forced NPC_FakeSense events with certainty computed from distance and loudness, hooked on
  START_NEXT_FRAME.
