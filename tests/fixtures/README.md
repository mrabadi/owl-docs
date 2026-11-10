# Native compatibility fixtures

These small DOCX files were generated locally by the frozen, fully validated
OwlDocs 0.6.0 Release binaries. The engine and academic fixtures were captured
before changing the built-in style definitions; the desktop style fixture was
then created using those untouched older Release libraries. They contain
synthetic text and no personal documents or external content.
Keep their bytes unchanged: generating them with the current writer would
remove the backward-compatibility guarantee.

| Fixture | Coverage | SHA-256 |
|---|---|---|
| `owldocs-0.6.0-styles.docx` | All 14 built-in paragraph styles, a native equation and a 2×2 table | `3b60f4caef36aa65b39b4ba27fbcb0256a9c94c282b92dab32155f85022e8789` |
| `owldocs-0.6.0-desktop-styles.docx` | The rich style fixture imported and saved from an unsaved desktop document by the untouched 0.6.0 Release libraries; includes the desktop's explicit equation/table font projection | `08f856c6518b055b3887b8b636a080280f7cb920f616f275414b4914a820675f` |
| `owldocs-0.6.0-academic.docx` | Actual desktop save/reopen/edit/save; Heading 1, multipage TOC cache, note, numeric citation, deleted caption/bibliography prefixes, hidden SEQ targets and visible REF fields | `151c54cd9a208b60fc0cc26b7e99de6147fed767571e7c91436c5410c09085a0` |
| `pre-regional-table-single-line-header.xml` | Exact single-line native header part captured before the independent-region table extension; protects the original tab-stop representation | `63ba5e3ac8454434365d0cd626cb142269bf4cf73747524c25c73df7b9e6882a` |
| `owldocs-0.6.1-styles.docx` | Frozen 0.6.1 neutral catalog: all 14 paragraph styles, equation and 2×2 table | `aa5eafb3156ba55409e6af1dab83eef6d890e7febf748f40a98a6c371616aa32` |
| `owldocs-0.6.1-desktop-styles.docx` | Frozen 0.6.1 desktop import/save of the neutral rich style fixture, including explicit equation/table font projection | `39a157b68f580e8f462be0611684558adcbb01173010a2d9b2a40172bb6902d7` |
| `owldocs-0.6.1-academic.docx` | Frozen 0.6.1 actual desktop save/reopen/edit/save of notes, TOC, citations and deleted-prefix native targets | `3a1002667cb462770f3cf0a9b14a125ffdf889c7f003f0122a1852664a8fbd12` |

The native style-profile test verifies exact style-part reproduction and rejects
modified style catalogs. The desktop workflow test verifies ordinary layout
edits save without a conversion prompt while preserving the older appearance
and academic identities.

The engine fixture also exposed a pre-existing engine-to-desktop projection
difference: the 0.6.0 desktop adds explicit default fonts to unstyled equations
and table cells. The old desktop already requires a supported-copy save for
that engine-only representation. It remains a native profile unit fixture;
the actual desktop regression uses the separate desktop-authored fixture.
The original package proof has not been broadened to ignore those differences.

The 0.6.1 fixtures were captured before the 0.6.2 style-color change, using a
locked snapshot of the final validated 0.6.1 public headers and static Release
libraries. The reference release package has SHA-256
`587e27c1c98281fddd8d9e73a5c50a70889d20491c0519339214a80a06461356`.
The academic capture executed its own source-proof, semantic-identity and later
ordinary-save checks against those frozen libraries. These fixture bytes must
also remain unchanged when the current writer changes.

The 0.6.2 table fixtures lock the previous table representation before the
0.6.3 geometry/border authoring change:

| Fixture | Coverage | SHA-256 |
|---|---|---|
| `owldocs-0.6.2-default-table.xml` | Exact automatic two-column native table XML captured with the unchanged 0.6.2 writer before authoring transport changes | `21585a883263f0b28324a6b53122e397ad9e1a2f6ac5598ce402087909658bdf` |
| `owldocs-0.6.2-desktop-table.docx` | Actual final clean 0.6.2 MainWindow table-picture workflow, two save/reopen cycles with PNG/JPEG payloads, ordinary default geometry and verified schema15 identity | `d967804c75b1205456476883a02e7740d548bcb509949dbd0dddadce57d0faa0` |

The desktop fixture is the exact native artifact recorded in the validated
0.6.2 release. Its source-proof save regression avoids the pre-existing bare
engine-to-desktop explicit-font projection difference described above. Keep
both fixtures immutable.
