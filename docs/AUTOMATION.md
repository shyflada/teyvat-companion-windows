# Offline automation research preview

Initial build **0.1.0-dev.4**, 2026-11-08. Local PNG sequences can detect a stable
closed-to-open transition, match a marker and commit collection/history without
a separate manual status action. This is an offline experiment, not a live
Genshin detector. Positions and their declared uncertainty come from the recording.
Minimap localization, camera tracking, capture, routes and official-map writes
were not implemented in dev.4. Dev.5 adds calibrated image-derived positions in
version 2 recordings; see [LOCALIZATION.md](LOCALIZATION.md). This guide describes
the compatible version 1 recording path.

## Try the bundled example

1. Run the extracted `TeyvatCompanion.exe` and open **Local map**.
2. Import `examples/automation/map.json`, accepting the review. Select
   **Original offline automation example** in the layer selector.
3. Choose **Analyze recording** and select `examples/automation/recording.json`.
4. Ten PNG frames produce two opening events. Both example markers become
   **Collected** automatically, with a results table and history entries.
5. **Undo progress** reverses both changes. Reanalyzing reports duplicates and
   respects the undo. Use a separate local profile for a fresh run.

The images are original synthetic artwork under MIT, not screenshots, official
assets or real chest coordinates. Import adds shared synthetic marker definitions;
collection and receipts are profile-specific. Ambiguous events can select their
candidates on the map for review without changing statuses. Existing manual
confirmation remains available there.

## Evidence and matching rules

- The calibrated object crop must match a closed reference for two consecutive
  frames, then an open reference for two consecutive frames. Opened frames alone,
  a reward panel alone, and single-frame flashes are insufficient.
- Similarity is one minus mean absolute RGB error divided by 255. Each recognized
  state requires at least 0.96 similarity and a 0.04 margin over the other state.
  Templates must be textured and have mutual similarity at most 0.90. These
  constants are research heuristics, not calibrated probabilities.
- Frames use strictly ordered integer milliseconds. A gap over 1000 ms resets
  pending evidence; opening must follow the last stable closed frame within
  5000 ms. A persistent open state emits one event until stable closed frames
  rearm the detector.
- Source, region, layer and coordinate-frame revision must match the local map.
  Mismatches fail before writes; no coordinate transform is guessed. Position
  must be inside the map.
- Automatic matching requires one candidate within radius plus position
  uncertainty, declared confidence at least 0.95, uncertainty at most a quarter
  of the radius, and movement between evidence positions at most half the radius.
  The marker's distance plus uncertainty must fit the radius. Archived/confirmed
  neighbors still count as candidates to avoid selecting another nearby marker
  merely because it remains uncollected.
- Only `common`, `exquisite`, `precious`, `luxurious` and `remarkable` categories
  can be automatically collected. Custom categories need review. Retired markers
  are not changed; confirmed statuses are preserved.

The program validates declared position fields, but cannot establish their actual
accuracy. Fixed crops require prior calibration and a stationary view. Arbitrary
resolutions, rotation, camera motion, scene cuts, lighting, languages and UI scales
are not validated. A similar unrelated object can fool template matching. This
prototype must not be presented as a validated game detector.

## Format and resource limits

See [the example manifest](examples/automation/recording.json). Required fields:
`format: "teyvat-offline-recording"`, integer `version: 1`, provenance, license,
map identifiers/frame, `match_radius` in local units, integer pixel `roi`, two
reference PNG filenames, and a `frames` array. Frames have `image`, `time_ms` and
optional `position` with `x`, `y`, `uncertainty`, `confidence`. Missing position
produces no automatic marker change.

Limits: 1 MiB JSON, 4–600 frames over at most ten minutes, 4 MiB per PNG, opaque
PNGs up to 1920×1080, reference/ROI dimensions 16–128 pixels per side, 64 MiB
compressed input over all reads. Resolution cannot change within a recording.
Only sibling PNG filenames are accepted: no URLs, traversal or symlinks. Images
are decoded sequentially on a worker. Input frames are not copied, saved or uploaded.

## Persistence and upgrades

Schema v4 adds `detection` history origins and `detection_receipts`. Accepted
changes, receipts and map revision commit in one SQLite transaction. Matching
uses current marker metadata inside that transaction. Failures roll back every
change. One Undo reverses a recording's automatic status changes.

Receipts contain event/content hashes, marker identity, scalar scores, recording
time, source/rights text and a command ID. They contain no pixels or account data.
Deduplication survives restart, Undo and full backup/restore and is profile-scoped.
Recording identity includes normalized JSON, referenced PNG bytes and algorithm
version. Formatting JSON or moving a bundle does not change it. Changing metadata,
times, filenames or encoded images can create a new identity. This is not global
physical-chest deduplication across independently edited recordings.

Existing v1/v2/v3 databases upgrade transactionally after a consistent snapshot,
preserving profiles, maps/progress, event IDs and undo links. Full restore still
requires current schema: retain older backups and their matching old application;
restore there and open the restored data with this version to migrate it. Newer
databases are never downgraded.

## Verification and remaining work

CTest covers 32 positive and 32 negative synthetic sequences, absent/jumping
positions, ambiguous/archived candidates, frame mismatches, repeated events,
restart, profiles, Undo, backup/restore, transactional failure and actual previous
preview migration. Fixture renderer, seeds and negative cases are in
`tests/detection_fixtures.hpp` and `tests/detection_tests.cpp`. Synthetic metrics
verify these implementation rules, not Genshin recognition accuracy.

Beta.4 additionally samples a bounded central scene crop near an official target.
The animation path requires interaction cues on multiple frames, a rising gold
burst with ring coverage >=0.70, and a blue dissolution increase on two frames.
It also requires a reliable recent pose, <=2-unit movement through the transition,
and the existing unique-marker/locked-target transactional guards. A completed
animation waits for the next genuinely matched position before writing progress.
It does not assign invented template scores. Schema v5 adds animation_receipts;
schema-v4 template constraints remain unchanged. Both receipt types deduplicate
replays and survive Undo, backup/restore and migrations.

The new supplied 28-frame opening sequence localizes on all 28 frames and marks
official point 4849 once. Three earlier non-opening sequences yield no animation
events (180 frames). This is a small development sample, not a held-out accuracy
estimate. Keyboard interaction cues, burst visibility, chest type, camera angle,
effects and UI scaling may still cause missed openings. F8 is a configurable
global fallback and always saves the current selected official ID locally first.

Next: evaluate authorized representative recordings with independently checked
labels and trustworthy positions; add object/scene continuity evidence; measure
event and marker errors on held-out regions/UI configurations. Assess permitted
official-map integration separately. The official website in a native browser
owns optional account requests. See [INTEGRATIONS.md](INTEGRATIONS.md).
