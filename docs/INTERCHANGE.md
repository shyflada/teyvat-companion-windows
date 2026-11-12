# Local interchange format v1

JSON and CSV transfer layer/marker metadata and the **active profile's** statuses.
Export includes the entire selected layer, including archived markers; filters do
not limit it. Settings, other profiles, history and PNGs require a full database backup.

The import dialog previews new layers, new/changed markers and progress changes.
Existing explicit local progress is preserved by default. File statuses apply
where no local progress record exists. Replacement is an explicit option. Undo
reverses imported progress as a group; it does not reverse imported metadata.

Parsing runs off the UI thread. Apply revalidates data and a token covering its
content, target profile, policy and database revision. A changed database rejects
the preview. Metadata, statuses and history commit in one transaction.

## JSON

Top-level fields: `format: "teyvat-companion"`, numeric `version: 1`, `maps` and
`markers` arrays. [examples/local-grid.json](examples/local-grid.json) is a working
synthetic coordinate example, not a source of real game-world locations.

Layer fields: `source`, `region`, `layer`, `title`, `frame`, `width`, `height`,
`provenance`, `license`, `data_version`. Marker fields: `source`, `region`, `layer`,
`id`, `category`, `label`, `notes`, `x`, `y`, `status`, boolean `retired`, `origin`.

Layer identity is `(source, region, layer)`; marker identity is `(source, id)`.
Manual IDs use `user:<UUID>` in the chosen map's namespace, with origin `manual`.
Files may declare `manual` or `import`; resulting progress-history events always
record origin `import`.

Coordinates use a declared local frame, X right/Y down, within `[0,width]` and
`[0,height]`. They are not metres or walking distances. Existing layer frames and
bounds are immutable: use a new layer identity for a changed transform. Stable
marker IDs may move to that new layer while keeping progress/history. Missing
records are retained, not silently retired/deleted. Use `retired: true` explicitly.
Re-importing an unchanged status creates no duplicate history event.

Source/licence strings are declarations, not proof of rights or authenticity.
Imports do not fetch URLs, load image/file references, execute content, inspect
browser state or synchronize with HoYoLAB.

## CSV

UTF-8, comma-separated, double-quote escaping and CRLF record endings. Exact header:

```text
format_version,record_type,source,region,layer,title,frame,width,height,provenance,license,data_version,id,category,label,notes,x,y,status,retired,origin
```

Each row has format_version `1` and record_type `layer` or `marker`. Layer rows use
layer fields; marker rows reference included layers and use marker fields. Unused
cells are empty. Numbers use decimal points; booleans are `true`/`false`. Notes
preserve embedded quotes, commas and newlines.

**Every data cell is prefixed with one ASCII apostrophe before CSV quoting,
including empty and numeric cells.** Import removes exactly one prefix. This
`apostrophe-v1` contract preserves leading apostrophes/zeroes and treats formula
prefixes as text. `=SUM(A1:A2)` becomes `"'=SUM(A1:A2)"`; `'name` becomes `"''name"`.
Fixed safe header names do not use the prefix. Spreadsheet programs may display
prefixes or change the contract on saving; JSON is easier for hand-authored data.

## Limits

- 8 MiB per document; 1024 layers and 5000 markers per transfer.
- 64 local layers, 5000 markers per layer and 20,000 markers in the database.
- Valid UTF-8/Unicode, required metadata, no duplicate layer/marker identities and
  each marker's layer included in the file.
- Finite dimensions above zero and at most 1,000,000 units; finite in-bounds coordinates.
- IDs: 128 UTF-16 units; category: 64; name: 120; notes: 2000; provenance/licence: 512.
  Controls are rejected except tabs/newlines in notes.
- Exact status codes: `not_collected`, `possibly_collected`, `collected`,
  `manually_confirmed`, `unknown`, independent of interface language.

Exports use QSaveFile temporary-file/commit semantics. Use a full database backup
for a layer exceeding interchange limits. Interchange is not a full backup or
official synchronization format.
