# Header/footer margin regressions — 0.6.2

The user reported that changing margins immediately makes header/footer text
disappear or shift. The stored text survives; the previous renderer clipped
stories to `margin - 36pt distance - 6pt gap`, which becomes zero at Narrow
margins. Body layout used the requested margin without reserving the story.

## Existing stories survive changing margins

- RED: built and ran `header_margin_tests` against the existing implementation.
  A two-line header and footer were visible at 72pt margins; switching to 36pt
  failed with `changing margins makes the existing header disappear`.
- GREEN: the same test passes after measuring all three story regions and
  reserving their rendered height in body pagination. The requested margins
  and semantic story text remain intact. Undo restores the previous margins.
- Shared page rendering and hit testing consume the same measured story
  geometry. Page-count fields are remeasured after pagination stabilizes.

## Typing and separation

- RED: the next executed test failed with `typing a multiline story with narrow
  margins is rejected instead of reserving body space`.
- GREEN: validation now considers available story capacity, rather than the
  story's previous height. The same test passes for newly typed three-line
  headers and footers at Narrow margins. A raster assertion verifies the first
  body line is below the header, and PDF extraction retains all story and body
  text without mutating the document. The focused run passed in 0.27 seconds.
- Bounded overflow remains rejected; imported oversized text stays editable
  when shortening it. The earlier fixed-30pt tests now use genuinely oversized
  stories beyond the expanded capacity. This is an intentional contract change
  requested by the margin-corruption feedback, not an exception to validation.

Unrelated compiler warnings in concurrent table picture changes were corrected
before rerunning the behavioral tests; those failures are not RED evidence.

## Pictures with Narrow margins

The next executed RED failed `narrow margins shrink a new header picture to an
invisible one-point dot`. Picture sizing was using the previous empty story's
zero height. Sizing now uses available story capacity; pagination reserves the
resulting image/text height. The same test passes, and the existing portrait
fixture retains its original 150pt height instead of shrinking to 30pt. Genuine
combined text/image overflow remains rejected. The related header/footer UI,
margin, application workflow and workspace controls run passed 4/4 in 5.20s.

The application integration target also saves/reopens the same three-region
multiline stories at 36pt, changes margins to 54pt, and saves/reopens again with
the requested geometry and exact source text intact.

Final focused checks also keep the on-page story editor open while changing
margins: the exact text, visible editor and caret position survive both changes.
Both `header_margin_tests` and `header_footer_ui_tests` pass at 150% display
scaling. An independent read-only review found no actionable geometry or
provenance defects; complete final Release gates are recorded separately.
