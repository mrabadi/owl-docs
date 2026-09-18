const { app, BrowserWindow, Menu, ipcMain, session } = require("electron");
const fs = require("fs/promises");
const path = require("path");

const MAX_REQUEST_BYTES = 32 * 1024 * 1024;
const MAX_SCENE_BYTES = 32 * 1024 * 1024;
const MAX_PREVIEW_BYTES = 16 * 1024 * 1024;
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

let mainWindow;
let request;
let completed = false;
let mode = "professional";

function optionValue(name) {
  const prefix = `${name}=`;
  const inline = process.argv.find((arg) => arg.startsWith(prefix));
  if (inline) return inline.slice(prefix.length);
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : null;
}

const sessionDirectory = optionValue("--owl-docs-figure-session");

function sessionPath(name) {
  if (!sessionDirectory) throw new Error("Missing Owl Docs figure session");
  return path.join(sessionDirectory, name);
}

async function writeAtomic(name, contents) {
  const destination = sessionPath(name);
  const temporary = `${destination}.tmp-${process.pid}`;
  await fs.writeFile(temporary, contents, { mode: 0o600 });
  await fs.rename(temporary, destination);
}

async function finish(result) {
  if (completed) return;
  completed = true;
  await writeAtomic("result.json", JSON.stringify({
    version: 1,
    token: request.token,
    mode,
    ...result
  }));
  app.quit();
}

function blockNetworkRequests() {
  session.defaultSession.webRequest.onBeforeRequest((details, callback) => {
    const allowed = details.url.startsWith("file:") ||
      details.url.startsWith("data:") || details.url.startsWith("blob:");
    callback({ cancel: !allowed });
  });
}

function installMenu() {
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    {
      label: "File",
      submenu: [
        { label: "Save Figure", accelerator: "CmdOrCtrl+S", click: () => mainWindow?.webContents.send("figure:save-request") },
        { label: "Cancel", accelerator: "Escape", click: () => finish({ saved: false }) }
      ]
    },
    { label: "Edit", submenu: [{ role: "undo" }, { role: "redo" }, { type: "separator" }, { role: "cut" }, { role: "copy" }, { role: "paste" }] },
    {
      label: "Settings",
      submenu: [{
        label: "Mode",
        submenu: [
          { label: "Sketch", type: "radio", checked: mode === "sketch", click: () => mainWindow?.webContents.send("figure:mode-request", "sketch") },
          { label: "Professional", type: "radio", checked: mode === "professional", click: () => mainWindow?.webContents.send("figure:mode-request", "professional") }
        ]
      }]
    }
  ]));
}

function createWindow() {
  const window = new BrowserWindow({
    show: !request.renderOnly,
    width: 1440,
    height: 920,
    minWidth: 860,
    minHeight: 600,
    title: request.title || "Excalidraw Figure — Owl Docs",
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      webSecurity: true,
      allowRunningInsecureContent: false
    }
  });
  mainWindow = window;
  window.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
  window.webContents.on("will-navigate", (event) => event.preventDefault());
  window.webContents.on("will-attach-webview", (event) => event.preventDefault());
  window.on("close", (event) => {
    if (completed) return;
    event.preventDefault();
    finish({ saved: false }).catch(() => app.exit(1));
  });
  window.loadFile(path.join(__dirname, "..", "dist", "index.html"));
}

app.commandLine.appendSwitch("disable-background-networking");
app.commandLine.appendSwitch("disable-component-update");
app.commandLine.appendSwitch("disable-features", "MediaRouter,OptimizationHints,AutofillServerCommunication,CertificateTransparencyComponentUpdater");
app.commandLine.appendSwitch("disable-gpu");

app.whenReady().then(async () => {
  if (!sessionDirectory || !path.isAbsolute(sessionDirectory)) {
    throw new Error("Owl Docs figure editor requires an absolute session directory");
  }
  const requestBuffer = await fs.readFile(sessionPath("request.json"));
  if (requestBuffer.length === 0 || requestBuffer.length > MAX_REQUEST_BYTES) {
    throw new Error("Figure request is empty or too large");
  }
  request = JSON.parse(requestBuffer.toString("utf8"));
  if (request.version !== 1 || typeof request.token !== "string" ||
      !/^[a-f0-9]{64}$/.test(request.token) ||
      (request.scene !== null && typeof request.scene !== "object") ||
      (request.skeleton !== undefined && !Array.isArray(request.skeleton)) ||
      (request.renderOnly !== undefined && typeof request.renderOnly !== "boolean")) {
    throw new Error("Invalid Owl Docs figure request");
  }
  mode = request.mode === "sketch" ? "sketch" : "professional";

  blockNetworkRequests();
  session.defaultSession.setPermissionRequestHandler((_, __, callback) => callback(false));
  session.defaultSession.setPermissionCheckHandler(() => false);
  ipcMain.handle("figure:request", () => ({
    scene: request.scene,
    mode,
    skeleton: request.skeleton || null,
    renderOnly: request.renderOnly === true
  }));
  ipcMain.handle("figure:set-mode", (_, nextMode) => {
    mode = nextMode === "sketch" ? "sketch" : "professional";
    installMenu();
    return { ok: true, mode };
  });
  ipcMain.handle("figure:commit", async (_, scene, previewBytes) => {
    if (completed) return { ok: false, error: "Session already completed" };
    if (typeof scene !== "string" || Buffer.byteLength(scene, "utf8") > MAX_SCENE_BYTES) {
      return { ok: false, error: "Figure scene exceeds the 32 MiB limit" };
    }
    let parsed;
    try { parsed = JSON.parse(scene); } catch { return { ok: false, error: "Figure scene is not valid JSON" }; }
    if (!parsed || !Array.isArray(parsed.elements)) {
      return { ok: false, error: "Figure scene has no element list" };
    }
    const preview = Buffer.from(previewBytes);
    if (preview.length === 0 || preview.length > MAX_PREVIEW_BYTES ||
        !preview.subarray(0, PNG_SIGNATURE.length).equals(PNG_SIGNATURE)) {
      return { ok: false, error: "Figure preview is not a bounded PNG" };
    }
    await writeAtomic("scene.excalidraw", scene);
    await writeAtomic("preview.png", preview);
    await finish({ saved: true });
    return { ok: true };
  });
  ipcMain.handle("figure:cancel", async () => {
    await finish({ saved: false });
    return { ok: true };
  });
  installMenu();
  createWindow();
}).catch(async (error) => {
  try {
    if (request?.token) await finish({ saved: false, error: String(error.message || error) });
  } finally {
    app.exit(1);
  }
});

app.on("window-all-closed", () => {
  if (!completed && request?.token) {
    finish({ saved: false }).catch(() => app.exit(1));
  }
});
