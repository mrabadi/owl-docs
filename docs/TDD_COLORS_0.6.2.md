# Color palette and style evidence for 0.6.2

The requested TDD workflow is applied one behavioral slice at a time. Shared
builds use `flock /tmp/owldocs-build.lock`.

## Swatch grid

The first configured command was:

```sh
flock /tmp/owldocs-build.lock cmake --build build-release --target color_palette_tests -j2
```

It executed RED at the deliberately new public API: `ColorPaletteWidget.h`
was absent. The acceptance test requires a substantial two-dimensional grid,
exact color selection, tooltip/accessibility labels and no visible per-swatch
color names. The reference PDF's exact light/dark colors and generated theme
tints/shades are the intended selectable values; section labels remain visible.

## Frozen style fixtures

Before changing any style definition, the final validated 0.6.1 public headers
and Release libraries were snapshotted under the build lock. The native engine,
desktop rich-style and academic fixtures listed in `tests/fixtures/README.md`
were generated using that frozen snapshot. The academic capture executed exact
semantic restore, source proof, native targets and ordinary later-save checks.
The 0.6.0 fixture bytes remain unchanged. No producer label authorizes a rewrite;
profile selection still requires exact style-part reproduction followed by the
complete package proof.

The first grid GREEN ran `ctest --test-dir build-release --output-on-failure
-R '^color_palette_tests$'`: 1/1 passed in 0.06 s. An initial Qt6 ambiguous
`mapTo` overload in the test was repaired as setup, and is not counted as RED.

The next executed runtime RED was “Right arrow does not move across theme
columns.” Geometric arrow navigation and Enter selection made the same target
GREEN in 0.06 s. The following missing-API RED covered Automatic/No Color,
custom choice, current-color indication, and persistent recent colors. The
implemented component passed in 0.11 s: caret updates do not emit or populate
history; actual remembered choices are deduplicated, bounded to ten, and kept
separate for text and highlight.

## Source style profiles and current colors

The new `neutral_style_defaults` member and frozen-neutral core lookup each
had an executed missing-symbol RED. The native fixture test then executed a
runtime RED: “0.6.1 neutral style profile was not detected.” Current core color
expectations executed 13 runtime failures for the previously neutral Title,
Subtitle and headings. Only then were the current core/native catalogs changed
to the supplied forest, green, blue, purple, brown and slate hierarchy. The
original 0.6.0 and 0.6.1 catalogs remain independent immutable profiles; exact
used-catalog bytes identify them, followed by the unchanged full-package proof.

## Ribbon integration and final verification

The actual Ribbon runtime RED was “font color is still a named menu instead of
the swatch grid.” The two committed-custom-color history methods also had an
executed missing-symbol RED. Text/highlight QMenus now contain the widget through
QWidgetAction, preserve their public menu identities for adaptive overflow, and
use the existing immediate document actions. Both color buttons retain 30×30
geometry, 22 px icons, keyboard access and mouse document-focus protection.

Further executed runtime REDs identified Enter not activating Automatic/custom
controls, popup opening without a focused usable choice, and the missing classic
yellow highlight choice. Minimal action-key handling, current-choice focus on
opening, and the retained classic highlighter swatches made the full component,
Ribbon, branding and application smoke group GREEN: 4/4 in 2.00 s. Existing color
route tests now click actual swatches instead of obsolete per-color QAction IDs;
they retain live custom-picker, selection, focus, clear-highlight, document-switch
and immediate-application assertions. The legacy mouse-focus assertion now also
requires Tab focus, while excluding Click focus.

Core and native profile checks passed 2/2 in 0.05 s. The extended desktop workflow
then passed actual open/layout-save/reopen for both 0.6.0 and 0.6.1 rich and academic
fixtures, application of the new green Heading2 while retaining the original
styles.xml, and both missing-source academic recovery/save paths under 18 pt
insertion preferences. Full OOXML, native profile and desktop workflow tests
passed 3/3 in 6.65 s. The recovery codec deliberately retains schema14 for these
ordinary documents, so no registry-proof exception or schema workaround was
needed.

Read-only normal and 150% component runs plus the actual Ribbon popup/Escape
check at 150% passed. The four captures under
`output/ui-review/0.6.2/colors/` were inspected: section grids, default/custom
choices and recent rows fit without clipping. Final clean Release/whole-suite
results are recorded separately in the release validation document.

The editor-tool bridge's inherited heading-color expectations were also updated
to read the current catalog after their observed failures. Three inherited
Heading1 locations changed; the explicit black span, explicit red span,
Normal/NoSpacing black and provenance/directness assertions remain unchanged.
The fresh bridge target passed 1/1 in 0.19 s. These are test-only expectation
updates for the intentionally changed defaults.

Final clean Release validation passed207/207 build steps and71/71 tests in33.23s.
The first whole run exposed five remaining import-test neutral inherited-color
expectations; only those expected authored/current Heading1/2 colors changed.
Foreign-source blue, explicit direct red/black, Normal black and all sparse
provenance checks remain. The focused import test passed3.56s, followed by the
complete71-test pass. Twelve fresh scaled cases and the exact old-style recovery
workflows also passed. See RELEASE_VALIDATION_0.6.2.md for final package gates.
