# OwlDocs 0.6.2 release validation

Validated on 2026-11-09 on Ubuntu 22.04.5 LTS with GCC 11.4.0, CMake 3.22.1
and Qt 6.2.4. This records the tested scope of this development release; it does
not establish general Word compatibility.

## Clean Release and complete tests

All production sources and public interfaces were frozen before the completely
clean strict Release build. All **207 build steps passed** and the freshly built
application reports `OwlDocs 0.6.2`. The bundled figure editor is required.

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DDOCXSTUDIO_STRICT_WARNINGS=ON \
  -DOWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE=ON \
  -DOWL_DOCS_CORPUS_MANIFEST=/tmp/owl-docs-public-corpus-060/manifest.json
flock /tmp/owldocs-build.lock cmake --build build-release --clean-first -j2
ctest --test-dir build-release --output-on-failure
```

The final complete run passed **71/71 tests, zero failures, in 33.23 seconds**.
The first complete run exposed legacy test expectations for neutral inherited
heading colors. Only five expected current-style colors in that import test
were corrected; foreign blue, direct red/black, Normal black and provenance
assertions remain intact. The focused import test and the complete suite were
then rerun. No production source or executable changed after the clean build.

The suite covers the new swatch grids, Automatic/No Color, keyboard popup
navigation, committed custom/recent choices, colored styles, 0.6.0 and 0.6.1
exact style profiles, source-preserving save and recovery, source fonts under
different insertion preferences, header/footer margin growth and active input,
71-test workspace/Codex/LaTeX and document regressions, table-cell PNG/JPEG
insertion/selection/formatting/clipboard/recovery, and native save/reopen/edit/save
identity and full-package reproduction proofs. The recovery codec keeps schema14
for ordinary documents and uses schema15 when cell pictures require it.

The pinned public LibreOffice corpus opened **100/100** DOCX fixtures and
produced **100/100 byte-identical unchanged copies**, with zero failures. The
fixture revision is `2776654bbb109af114a44fe00ec9771e4ad2d99d`; the manifest pins
Git blob identities and SHA-256 digests. This is an unchanged-copy corpus gate;
it does not claim arbitrary editing coverage. The corpus is not packaged.

## Native documents and real figure rendering

Two academic/header fixtures generated against the fresh Release libraries
opened in LibreOffice 7.3.7.2 and produced two-page PDFs. Each deliberately had
five REF caches changed to `STALE`; all five recomputed correctly. Decimal
footnote/endnote sequences, deleted-prefix caption/bibliography targets, full-text
references, safe cases and centered matrix notation, original owl artwork and
regional story alignment were checked in extracted text, native XML and renders.
Header text bounds begin at 36.25 pt and the aligned footer text at 742.85 pt.

A third fixture was saved by the fresh actual MainWindow workflow. It retained
exact table/cell/picture identities, PNG/JPEG payloads, alt text and geometry
through two save/reopen cycles. LibreOffice rendered its one-page Letter output
in the exact order `Prefix Before [picture] between [picture]`, with both pictures
at **381000×228600 EMU (30×18 pt)**. Native and screen/PDF raster checks found no
filename flattening, overlap or clipping. Unsupported floating cell pictures
reject before target replacement; malformed/external cell pictures retain their
original native content while safe outside text patches remain available.

The fresh opt-in real Excalidraw Professional renderer and Codex preview/accept
gate passed. The ordinary attempt encountered the outer tool sandbox's
NETLINK_ROUTE restriction; the scoped approved rerun passed while preserving the
helper's own bubblewrap isolation and bounds. LibreOffice used private temporary
profiles for native verification. No application installation or user-document
replacement was performed by these checks.

## Scaled UI and visual review

Twelve direct fresh Release cases passed at **150% scale**, with zero failures:
palette, Ribbon, workspace, workspace controls, margins, feedback workflow,
table-picture render/clipboard/native workflow and three capture groups.
Private settings, data and capture paths avoid interference with the full CTest
run. Representative fresh 820×600 and 1280×860 views, contextual rows, swatch
popup and native PDF renders were inspected with no unresolved visual findings.
Final captures are under `output/ui-review/0.6.2/final/`; the earlier normal and
scaled UI/TDD captures remain available beside them.

## Package verification

The artifact is `owl-docs_0.6.2_amd64.deb`. Final hashes and the extraction check
record live outside the package in `dist/SHA256SUMS` and
`dist/validation-0.6.2.json`, avoiding a self-referential package hash.
Verification requires correct package metadata/version, resolved main/parser
runtime libraries, byte-exact freshly stripped main/parser executables, a
packaged-parser native preflight, all eight original icon assets, all 73 bundled
helper files and byte-exact installed documentation. The accepted 0.6.1 artifact
and its SHA-256 digest are retained. Existing settings and recovery storage
identities remain unchanged.

See `RELEASE_NOTES_0.6.2.md`, `COMPATIBILITY.md`, `PRIVACY.md` and the installed
TDD evidence for the supported scope and remaining boundaries.
