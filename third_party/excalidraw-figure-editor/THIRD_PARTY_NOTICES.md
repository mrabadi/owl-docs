# Third-party notices

The Owl Docs figure editor is derived from the local Excalidraw fork at commit
`d2f65b9b930d9d727cfaec11c8b7123c771452b2`.

The locked build currently resolves these primary components:

- `@excalidraw/excalidraw` 0.18.1 — MIT
- React and React DOM 18.3.1 — MIT
- Vite 5.4.21 and its React plugin — MIT
- Electron 32.3.3 — MIT; the packaged runtime also carries Electron's license
  and Chromium's generated third-party license inventory.

`package-lock.json` is the authoritative dependency lock. Release publication
still requires the roadmap's complete SPDX SBOM and license audit; this notice
does not replace those gates.
