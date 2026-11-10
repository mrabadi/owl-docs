import assert from "node:assert/strict";
import test from "node:test";
import { figureExportOptions } from "../src/figureExportOptions.mjs";

test("ordinary figure export retains white paper and current app files", () => {
  const appState = { viewBackgroundColor: "#123456", zoom: { value: 2 } };
  const files = { current: { dataURL: "data:image/png;base64,AA==" } };
  for (const request of [{ version: 1 }, { version: 2, scene: {} }]) {
    const output = figureExportOptions(request, appState, files);
    assert.deepEqual(output.appState, { ...appState, viewBackgroundColor: "#ffffff", exportBackground: true });
    assert.equal(output.files, files);
  }
  assert.equal(appState.viewBackgroundColor, "#123456");
});

test("native export uses source background and embedded images before UI hydration", () => {
  const request = { version: 2, scene: {
    appState: { viewBackgroundColor: "#19352b", sourceOnly: "retained" },
    files: { original: { dataURL: "data:image/png;base64,AA==", retained: true } }
  } };
  const original = structuredClone(request);
  const options = figureExportOptions(request, { viewBackgroundColor: "#ffffff", currentOnly: true }, {});
  assert.equal(options.appState.viewBackgroundColor, "#19352b");
  assert.equal(options.appState.exportBackground, true);
  assert.equal(options.appState.sourceOnly, "retained");
  assert.deepEqual(options.files, request.scene.files);
  assert.deepEqual(request, original);
});
