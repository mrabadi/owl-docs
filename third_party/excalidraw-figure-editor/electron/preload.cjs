const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("owlDocsFigure", {
  request: () => ipcRenderer.invoke("figure:request"),
  setMode: (mode) => ipcRenderer.invoke("figure:set-mode", mode),
  commit: (scene, previewBytes) => ipcRenderer.invoke("figure:commit", scene, previewBytes),
  cancel: () => ipcRenderer.invoke("figure:cancel"),
  onSaveRequest: (callback) => {
    const listener = () => callback();
    ipcRenderer.on("figure:save-request", listener);
    return () => ipcRenderer.removeListener("figure:save-request", listener);
  },
  onModeRequest: (callback) => {
    const listener = (_, mode) => callback(mode);
    ipcRenderer.on("figure:mode-request", listener);
    return () => ipcRenderer.removeListener("figure:mode-request", listener);
  }
});
