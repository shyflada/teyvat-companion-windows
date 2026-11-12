# Contributing

Read SPEC.md and docs/ROADMAP.md before changing scope. Implement one phase at a
time. Phase 3 adds the local map, progress, interchange and backup workflows;
Phase 4 overlay is next. Run scripts/build.ps1 for the app/integration tests and
scripts/build.ps1 -CoreOnly for the independent domain core.

Use C++20, CMake and native platform/Qt facilities before adding dependencies.
Keep the portable core independent of Qt and Windows. Preserve existing IDs,
progress and history when introducing persistence or data updates.

Run the README build and CTest commands for core changes. Add focused tests for
data-loss paths, imports, migrations, uncertain evidence and coordinate/layer
mismatches. Assertions compiled out in Release are not adequate test checks.
Record what was actually run and what still needs Windows/manual verification.

Do not add game memory access, DLL injection, hooks, game-input automation,
traffic interception, private HoYoLAB endpoints or account-secret handling.
New datasets/assets require documented origin, licence, attribution and permitted
uses. Capture requires accuracy, compatibility and consent review before
enablement; offline fixtures require usage rights too.

Keep serialized values independent of translations. Do not commit local progress,
secrets, captures or build artifacts. Update documentation and the changelog when
behavior changes. Public publishing requires the project owner's confirmation.

No upstream repository or contribution inbox is configured yet. Local changes
can be reviewed directly; issue/PR templates and CI will be added with the
repository/release setup rather than linking to an invented GitHub organization.
