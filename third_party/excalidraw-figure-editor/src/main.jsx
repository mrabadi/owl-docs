import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { Excalidraw, convertToExcalidrawElements, exportToBlob, serializeAsJSON } from "@excalidraw/excalidraw";
import "@excalidraw/excalidraw/index.css";
import "./style.css";

window.EXCALIDRAW_ASSET_PATH = new URL("./", window.location.href).href;

const PROFESSIONAL_APP_STATE = {
  currentItemRoughness: 0,
  currentItemFontFamily: 10,
  currentItemArrowType: "elbow",
  currentItemStrokeColor: "#222624",
  currentItemBackgroundColor: "transparent",
  currentItemFillStyle: "solid",
  viewBackgroundColor: "#ffffff"
};
const SKETCH_APP_STATE = {
  currentItemRoughness: 1,
  currentItemFontFamily: 5,
  currentItemArrowType: "round",
  currentItemStrokeColor: "#1e1e1e",
  currentItemBackgroundColor: "transparent",
  currentItemFillStyle: "hachure",
  viewBackgroundColor: "#ffffff"
};

function appStateForMode(mode) {
  return mode === "sketch" ? SKETCH_APP_STATE : PROFESSIONAL_APP_STATE;
}

function professionalSkeleton(elements) {
  return (elements || []).map((element) => ({
    ...element,
    roughness: 0,
    fillStyle: "solid",
    strokeColor: element.strokeColor || "#222624",
    backgroundColor: element.backgroundColor || "transparent",
    ...(element.type === "text" ? { fontFamily: 10 } : {}),
    ...(element.label ? { label: { ...element.label, fontFamily: 10 } } : {})
  }));
}

function initialSceneForMode(scene, mode, skeleton) {
  const generated = skeleton
    ? { elements: convertToExcalidrawElements(professionalSkeleton(skeleton), { regenerateIds: false }) }
    : {};
  return {
    ...generated,
    ...(scene || {}),
    appState: {
      ...(scene?.appState || {}),
      ...appStateForMode(mode),
      // A serialized selection makes Excalidraw show that object's style
      // instead of the active mode's creation defaults when the editor opens.
      selectedElementIds: {},
      selectedGroupIds: {},
      editingGroupId: null
    }
  };
}

function App() {
  const apiRef = useRef(null);
  const [request, setRequest] = useState(null);
  const [mode, setMode] = useState("professional");
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");
  const [apiReady, setApiReady] = useState(false);
  const renderStartedRef = useRef(false);

  const initialData = useMemo(() => {
    if (!request) return undefined;
    return initialSceneForMode(request.scene, request.mode, request.skeleton);
  }, [request]);

  useEffect(() => {
    window.owlDocsFigure.request().then((value) => {
      const initialMode = value.mode === "sketch" ? "sketch" : "professional";
      window.EXCALIDRAW_LOCAL_SET_PALETTE?.(initialMode === "professional");
      setMode(initialMode);
      setRequest(value);
    }).catch((value) => setError(String(value)));
  }, []);

  const setApi = useCallback((api) => {
    apiRef.current = api;
    api.updateScene({ appState: appStateForMode(mode) });
    setApiReady(true);
  }, [mode]);

  useEffect(() => {
    if (!apiRef.current || !request) return;
    apiRef.current.updateScene({ appState: appStateForMode(mode) });
  }, [request, mode]);

  const selectMode = useCallback(async (nextMode) => {
    const resolvedMode = nextMode === "sketch" ? "sketch" : "professional";
    window.EXCALIDRAW_LOCAL_SET_PALETTE?.(resolvedMode === "professional");
    setMode(resolvedMode);
    apiRef.current?.updateScene({ appState: appStateForMode(resolvedMode) });
    await window.owlDocsFigure.setMode(resolvedMode);
  }, []);

  useEffect(() => window.owlDocsFigure.onModeRequest(selectMode), [selectMode]);

  const save = useCallback(async () => {
    const api = apiRef.current;
    if (!api || saving) return;
    setSaving(true);
    setError("");
    try {
      const elements = api.getSceneElementsIncludingDeleted();
      const activeElements = api.getSceneElements();
      const appState = api.getAppState();
      const files = api.getFiles();
      const scene = serializeAsJSON(elements, appState, files, "local");
      const preview = await exportToBlob({
        elements: activeElements,
        appState: { ...appState, exportBackground: true, viewBackgroundColor: "#ffffff" },
        files,
        mimeType: "image/png",
        exportPadding: 24,
        maxWidthOrHeight: 4096
      });
      const result = await window.owlDocsFigure.commit(
        scene, new Uint8Array(await preview.arrayBuffer()));
      if (!result.ok) throw new Error(result.error || "Could not save figure");
    } catch (value) {
      setError(value instanceof Error ? value.message : String(value));
      setSaving(false);
    }
  }, [saving]);

  useEffect(() => window.owlDocsFigure.onSaveRequest(save), [save]);

  useEffect(() => {
    if (!apiReady || !request?.renderOnly || renderStartedRef.current) return;
    renderStartedRef.current = true;
    // Let Excalidraw finish its first scene commit before exporting the local
    // PNG and native scene for the Owl Docs preview branch.
    requestAnimationFrame(() => requestAnimationFrame(() => save()));
  }, [apiReady, request, save]);

  return <div className="owl-figure-shell">
    <div className="owl-figure-toolbar">
      <div className="owl-figure-title">Excalidraw Figure <span>in Owl Docs</span></div>
      <div className="owl-mode-switch" role="group" aria-label="Drawing mode">
        <button className={mode === "sketch" ? "active" : ""}
          aria-pressed={mode === "sketch"} disabled={saving || !request}
          onClick={() => selectMode("sketch")}>Sketch</button>
        <button className={mode === "professional" ? "active" : ""}
          aria-pressed={mode === "professional"} disabled={saving || !request}
          onClick={() => selectMode("professional")}>Professional</button>
      </div>
      {error && <div className="owl-figure-error" role="alert">{error}</div>}
      <button className="secondary" disabled={saving} onClick={() => window.owlDocsFigure.cancel()}>Cancel</button>
      <button className="primary" disabled={saving || !request} onClick={save}>{saving ? "Saving…" : "Save Figure"}</button>
    </div>
    <div className="owl-figure-canvas">
      {request && <Excalidraw excalidrawAPI={setApi} theme="light" initialData={initialData} />}
    </div>
  </div>;
}

createRoot(document.getElementById("root")).render(<App />);
