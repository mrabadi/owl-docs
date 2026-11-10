# OwlDocs 0.6.2

This feedback update makes the writing controls easier to reach, applies the
provided Harvill colors to document styles, improves table insertion, and fixes
headers and footers when margins change. The original owl artwork is retained.

- Font and highlight colors use unlabeled swatch grids with theme lightness
  variations, reference-palette colors, current selection, keyboard navigation,
  recent colors and a custom picker. Automatic restores the paragraph's
  inherited text color; No Color removes highlighting.
- Title, Subtitle and Heading 1–9 use a coordinated forest/green/slate palette
  with restrained blue, purple and brown accents. Existing 0.6.0 and 0.6.1
  documents retain their original style parts through complete package proof.
- The larger File, Edit, Insert, Format, Layout, References, Review and View
  labels align vertically with the OwlDocs logo.
- Lists, tables, pictures, editable figures and equations expose their controls
  in a row below the primary writing bar. Picture wrapping and equation
  placement are direct choices. More appears only for controls that overflow.
- Insert Table offers a hover-highlighted 10×8 dimension grid. One click
  inserts; larger manual dimensions, a header-row option and keyboard access
  remain available. Cancellation leaves the document unchanged.
- PNG/JPEG pictures can be inserted inline in supported table cells, rendered,
  selected, resized, named and deleted. Their bytes and identities participate
  in undo, recovery, clipboard and native DOCX transport. Floating placement
  and equations inside cells remain outside the supported table subset.
- Header/footer layout reserves space for existing and newly typed stories.
  Narrow margins no longer clip them to zero height. Screen, input, body
  pagination and PDF use the same geometry. Requested margins remain stored
  independently of the required story height.
- The SQLite recovery journal remains schema 14. Semantic checkpoints use
  version 15 only when table-cell pictures require it; ordinary checkpoints
  keep the version-14 representation. Older checkpoints remain readable.

The release includes the regression evidence in `TDD_*_0.6.2.md`, a click audit,
the compatibility matrix and the final validation record. The prior release
artifacts remain available alongside this package.

## Install

```sh
cd /home/rabadi/Documents/ChatGPT/word
sudo apt install ./dist/owl-docs_0.6.2_amd64.deb
```
