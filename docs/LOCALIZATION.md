# Image localization and read-only evaluation

**Current beta.6, 2026-11-10:** official-map navigation supports automatic
minimap acquisition and retry after losses, preferred-region search and strict
full-resolution verification of coarse proposals. A saved last XY only guides
search. Narrow coastal matches require independent dense image agreement and
remain explicitly approximate with automatic collection blocked. Nearby official
floor overlays can be matched around a known hint; ambiguous planes abstain.
Bright snow is retained in both global and local terrain masks. Sparse coarse
proposals require stronger independent detailed evidence. Camera-heading formulas
are unchanged. See [RC3_REPORT.md](RC3_REPORT.md) for the current Windows preview.
The earlier checkpoint results and calibrated offline workflow below are historical.

**Beta.2 update:** game feedback confirmed a rotating minimap. The beta.1 pack
had declared only angle 0 (a +/-5 degree gate), so most rotated matches were
discarded. ORB configurations now accept optional boolean `allow_rotation` for
continuous angles. Weak ORB matches may fall back to SIFT from existing OpenCV;
the same geometry and ambiguity checks apply. An 81-case controlled rotation
regression on nine private recording frames accepts 81/81, maximum deviation
0.786 reference pixels from each original estimate, maximum angle deviation
0.581 degrees. This is transformed-image consistency, not independent position
accuracy or a live-game pass. HUD checks remain mandatory for game setups.
Correlation setups reject unrestricted rotation. The historical beta.1 results
below describe the previous checkpoint.

Historical checkpoint: **0.1.0-beta.1**, 2026-11-08. Foreground-only Windows capture
supports a private 2560x1440 Mondstadt calibration. OpenCV 4.12.0 ORB, mutual
Hamming/ratio matches and RANSAC similarity geometry complement the original
correlation method. Acceptance requires >=12 inliers, >=50% support, spatial
spread, RMS <=1.5 reference pixels, declared scale/rotation, reference coverage
and a textured fixed-HUD presence check. Alternate supported positions abstain.
Scores are heuristic, not calibrated probabilities. No camera heading or metres.

Two private game recordings were inspected. In the first, 31/35 gameplay frames
matched and all 25 map/transition frames abstained. In the second, 29/30 gameplay
frames matched and all 7 map/transition frames abstained. These are coverage
counts in one small area, not independent coordinate-accuracy measurements.
Opening templates were selected from the second recording: one opening sequence
detected there, zero in the first negative recording. Two nearby legacy markers
make the actual opening ambiguous: no progress changes. This single-view
calibration does not validate detection on other chests. Original synthetic ORB
tests cover rotation/scale, geometry, HUD absence and cancellation.

The historical dev.5 correlation-only instructions/results below remain useful
for the original synthetic fixture; their old implementation status is superseded.

Build **0.1.0-dev.5**, 2026-11-08. Version 2 recordings derive positions from
minimap pixels. They reject supplied `position` fields; evaluation labels live in
a separate file and are never inputs to localization. Version 1 recordings retain
their supplied-position behavior and content identity. Schema remains v4.

At the dev.5 checkpoint, only original synthetic fixtures were available.
Official-map writes and independent real-game accuracy evaluation remain pending.

## Try automatic positioning

1. Import `examples/localization/map.json` in **Local map** and select
   **Minimap localization example**.
2. Choose **Analyze recording** and open `examples/localization/recording.json`.
3. The application locates all ten example minimaps on the reference and records
   two chest openings automatically. Open the **Positions from images** results
   tab for coordinates, scores and abstention reasons. A gold ring shows the
   last recording position, never a live player position.
4. Optionally attach `examples/localization/reference.png` as the map background,
   with source `Original synthetic terrain` and rights `MIT`.
5. Undo and replay protection behave as before. Another local profile provides a
   fresh collection run. **Cancel analysis** stops computation before writing;
   once the final short transaction begins, it completes atomically.

## Calibration and matching

The v2 manifest adds a `localization` object with `reference`, `provenance`,
`license`, square minimap `roi`, `origin`, `units_per_pixel`, `scales` and
`rotations`. See [the complete example](examples/localization/recording.json).
Reference pixel center `(x,y)` maps to local coordinates
`origin + (x*units_per_pixel.x, y*units_per_pixel.y)`; these are not metres.
Scale is reference pixels per minimap pixel. Positive angles map minimap offsets
clockwise into the reference. Only the explicitly listed poses are searched.
Calibration must use the recording's source/region/layer/frame, and reference
coverage must fit the local layer. No world coordinate system is invented.

The matcher compares grayscale samples in an annulus: the central 28% of the
radius and outer 10% are ignored to reduce interference from a player indicator
and frame. It uses mean-normalized correlation, a four-pixel coarse search,
eight spatially separated seeds per pose, and one-pixel refinement. A full match
requires score at least **0.97**, a **0.06** margin against another spatial peak
more than six reference pixels away, and sufficient texture variance. Similar
locations, weak evidence or poor texture return **position unknown**.
The search is approximate: the retained runner-up is not proof of global uniqueness.

Mathematical reference: [OpenCV's normalized centered correlation formula](https://docs.opencv.org/4.13.0/de/da9/tutorial_template_matching.html).
That original correlation implementation uses Qt pixels and C++ math. The beta
also links minimal OpenCV for ORB. See [QImage pixel access](https://doc.qt.io/qt-6/qimage.html).
Correlation is a similarity score, not a calibrated probability. Position
uncertainty is a two-reference-pixel heuristic in local units, not a verified
physical error bound. Bounds checks cannot establish that calibration is correct.

Limits: reference 96–384 pixels per side, square minimap ROI 48–128 pixels,
at most 16 scale/rotation pairs, scales 0.5–4, at most 60 localized frames.
Existing bounded PNG/JSON/total-input limits apply. The chest and minimap crops
must not overlap. Identical minimap crops share one result within a run. Work
runs off the UI thread and checks cancellation; pixels/position traces are not
persisted in the database. The existing receipt stores scalar accepted-event
evidence only.

This is a calibrated local tile matcher. It does not discover an arbitrary
minimap boundary, search the entire game world, continuously estimate unknown
scale/rotation, identify floors/regions, distinguish arbitrary scene cuts or
verify the map's origin/version. The chest detector still requires fixed,
calibrated object crops. Incorrect but in-bounds calibration and visually similar
content can still produce wrong positions. Release enablement needs representative
recordings, independently verified labels and an explicit accuracy review.

## Evaluation without changing progress

From the extracted application folder, run:

```powershell
.\TeyvatEvaluate.exe --recording examples/localization/recording.json --dataset examples/localization/map.json --labels examples/localization/labels.json --output evaluation.json
```

Choose a new output filename; existing files are preserved. The tool does not
open the application database or write progress. Its report includes content hash,
per-frame positions/errors, accepted/abstained counts, mean/RMS/max error in local
units, event TP/FP/FN, precision/recall, wrong marker predictions and automatic
predictions on unmatched events. Undefined ratios are `null`, not invented scores.

[Labels](examples/localization/labels.json) use format `teyvat-evaluation-labels`,
version 1, `position_tolerance`, `event_tolerance_ms`, a position label for every
frame time, and ordered opening labels with `time_ms` and `marker_id`. A position
label can be null when localization should abstain. Event matching maximizes the
number of time-compatible one-to-one pairs, minimizing timing error as a tie-breaker.
It reports wrong marker identity separately from a correctly timed opening.
Annotation mistakes remain possible; independent review is required.

## Recorded synthetic checks

- 32 clean unseen-seed views with four configured rotations and two scales:
  32 accepted, at most one local unit error.
- 32 additional views with subpixel shifts, noise and brightness change:
  27 accepted, five abstained; maximum accepted error **0.85 local units**.
  Tests permit abstention while requiring accepted estimates to remain within
  two units. No coverage threshold for real-game release has been established.
- Blank, unrelated, heavily occluded, uncalibrated-pose and repeated-location
  examples abstain. Missing minimaps produce no collection; reference coverage
  outside layer bounds is rejected before writes.
- The ten-frame example derives all coordinates from pixels and automatically
  collects two markers. Tests cover its UI pointer/trace, cancellation, Undo,
  durable deduplication, and read-only CLI output preservation.
- Intentionally wrong position/marker/time labels yield nonzero errors and
  false events. Incomplete labels fail; empty precision/recall denominators are
  null. Maximum temporal matching is checked against a greedy-matching failure.

Reproduce the normal build with `scripts/build.ps1`. Optional environment variables
`TEYVAT_PHASE2_EXE`, `TEYVAT_PHASE3_EXE`, `TEYVAT_SCREENSHOT_DIR`,
`TEYVAT_LOCALIZATION_EXAMPLE_DIR` and `TEYVAT_LOCALIZATION_METRICS_PATH` enable the
previous-binary, screenshot and generated-artifact checks. All were supplied for
the recorded full verification run. Synthetic results do not establish Genshin
accuracy, compatibility or account safety.
