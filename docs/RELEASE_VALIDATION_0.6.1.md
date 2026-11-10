# OwlDocs 0.6.1 release validation

Validated on 2026-11-09 on Ubuntu 22.04.5 LTS with GCC 11.4.0,
CMake 3.22.1 and Qt 6.2.4. This records the tested scope of this development
release; it does not establish general Word compatibility.

## Clean Release and complete tests

All production sources and public interfaces were frozen before a completely
clean strict Release build. All **186 build steps passed**, with the bundled
figure editor required. The executable reports `OwlDocs 0.6.1`.

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DDOCXSTUDIO_STRICT_WARNINGS=ON \
  -DOWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE=ON \
  -DOWL_DOCS_CORPUS_MANIFEST=/tmp/owl-docs-public-corpus-060/manifest.json
flock /tmp/owldocs-build.lock cmake --build build-release --clean-first -j2
ctest --test-dir build-release --output-on-failure
```

The final complete run passed **59/59 tests, zero failures, in 31.15 seconds**.
This includes source-preserving saves/recovery, legacy 0.6.0 styles and academic
identities, source fonts under different preferences, adjacent story-image
identity, native equations and references, regional header/footer transport,
100-page field-driven wrapping, IME/clipboard input, workspace controls,
unchanged icon generation and the installed Codex protocol schema.

The pinned public LibreOffice corpus opened **100/100** DOCX fixtures and
produced **100/100 byte-identical unchanged copies**, with zero failures.
The fixture source is commit
`2776654bbb109af114a44fe00ec9771e4ad2d99d`; Git blob identity and SHA-256
checks qualify the downloads. The external corpus is not shipped in the package.

## Native DOCX and real figure renderer

Two synthetic native fixtures were generated against the freshly rebuilt
Release libraries. LibreOffice 7.3.7.2 opened them, refreshed fields and exported
two-page PDFs. Both contain native cases and a centered ordinary matrix,
TOC/captions/bookmarks, manual bibliography/citations, footnotes/endnotes and
original owl artwork. Both independent note sequences display decimal `1`.

Five REF caches in each fixture were deliberately changed to `STALE` before
refresh. All five recomputed correctly: caption and bibliography references
survive deleted visible prefixes, hidden sequence targets stay hidden, and a
full-text reference contains the remaining caption prose. Extracted text,
native XML and rendered page images were checked.

The two story fixtures cover explicit line breaks and natural wrapping without
line breaks. Their native borderless tables align header regions to the top
and footer regions to the bottom, matching the supported 36-point distances.
PDF text bounds place header center/right on the first left line and footer
page/right content on the final left line. Ordinary single-line native parts
remain byte-stable; older sequential multiline/wrapped parts retain their
conservative supported-copy boundary.

The opt-in real Excalidraw test passed with
`OWL_DOCS_TEST_LOCAL_EXCALIDRAW_RENDERER=1`, covering actual offline Professional
rendering, editable PNG scene metadata and Codex preview/accept. Its ordinary
attempt was stopped by the outer tool sandbox's NETLINK_ROUTE restriction;
a scoped approved run allowed the helper's own bubblewrap network namespace
to operate. This was a test-environment restriction, not a disabled helper gate.

## Scaled UI and final visual review

Eight direct Release test groups passed again at 150% scale, with zero
failures: header/footer input, clipboard, keyboard, toolbar, IME, field-driven
wrap workflow, workspace UI and workspace controls. Fresh 820×600 and 1280×860
workspace captures, narrow/wide story controls, labels and Canvas/PDF output
were inspected with isolated settings and capture directories.

The first visual review caught a partly covered Header label. A raster
regression drove its gutter placement, and a separate Narrow-margin regression
removed invented minimum story space: new content is rejected when no margin
space is available, while retained source can be shortened or cleared. Both
fixes passed the focused normal/150% matrix before the final completely clean
build and 59-test run recorded above.

A final synthetic Footer capture briefly omitted a toolbar row. A read-only
stability probe checked four real widget grabs and four synthetic renders after
queued layout events: both rows, all six controls, Page Count and Close were
visible and contained in every frame. The settled captures were inspected;
no persistent product issue or additional source change was found.

## Package verification

The artifact is `owl-docs_0.6.1_amd64.deb`. The final digest and extraction
verification record live outside the package in `dist/SHA256SUMS` and
`dist/validation-0.6.1.json`, avoiding a self-referential package hash.
Verification requires correct package metadata/version, resolved main/parser
runtime libraries, byte-exact freshly stripped main/parser executables, the
stripped parser processing a native fixture, all eight
original icon assets, all 73 bundled helper files, visible one-word branding
and byte-exact installed documentation. It does not install a replacement
application or alter existing settings/recovery storage.

See `RELEASE_NOTES_0.6.1.md`, `COMPATIBILITY.md`, `PRIVACY.md` and the TDD evidence
for supported behavior and remaining boundaries.
