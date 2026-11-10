# Click audit — OwlDocs 0.6.2

Counts start from the relevant existing selection. Choosing a particular value
inside an intrinsic picker is counted separately from opening it. File-dialog
navigation and text entry depend on the user's content and are not counted.

| Task | Prior feedback build | 0.6.2 route |
|---|---|---|
| Bold/italic/underline, bullets, numbering, alignment | Some formatting hidden behind More | One direct writing-bar command whenever it fits; only physically overflowed controls move to More |
| Change paragraph style/font/size | Primary chooser | Primary chooser retained, with theme-colored style defaults |
| Select a font/highlight color | Named menu rows | Open color picker, click exact swatch; shades and current selection are visible |
| Reuse a custom color | Reopen custom dialog | Open palette, click recent swatch |
| Table row/column edits | Open redundant Table wrapper, choose command | One contextual command below the writing bar |
| Picture wrapping | Open object wrapper, then Wrap Text | One direct Inline/Square/Top and Bottom command |
| Size/alt text/layout detail | Open object wrapper, then dialog | One direct contextual dialog command |
| Edit an Excalidraw figure | Object menu or double-click/F2 | One contextual Edit Figure command, plus existing double-click/F2 |
| Edit an equation | Context menu or double-click/F2 | One contextual Edit Equation command, plus existing double-click/F2 |
| Equation inline/display placement | Open editor and change mode | One direct contextual choice |
| Insert a small table | Open insertion dialog, enter two dimensions, confirm | Insert > Table, hover dimensions, one grid click; keyboard arrows and Enter also work |
| Larger table dimensions | Two numeric inputs | Direct row/column inputs below the quick grid |
| List nesting/properties | Redundant List wrapper | Direct contextual level, indent and Properties controls |
| Margins/paper/orientation | Inaccessible hidden legacy page controls | Labeled Layout menu routes |
| Codex model/reasoning/speed | Previous disclosure button | Settings remain directly visible; opening chat starts connection |
| Find/replace/outline | Navigation pane | Existing labeled commands and shortcuts retained |
| Save and state | Toolbar plus routine popups in older versions | One Save command, persistent save status; real conflict handling retained |

At 1280 pixels all primary formatting commands fit and More disappears. Narrow
rows are measured from their actual controls rather than a fixed width rule.
Contextual rows wrap instead of adding object wrapper menus. Mouse clicks keep
document focus; Tab and Enter expose the same commands to keyboard users.

Final rendered captures and exercised route checks are listed in
`TDD_WORKSPACE_0.6.2.md` and `RELEASE_VALIDATION_0.6.2.md`.
