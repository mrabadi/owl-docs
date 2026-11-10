export function figureExportOptions(request, appState, files) {
  const source = request?.version === 2 ? request.scene : null;
  return {
    appState: {
      ...appState,
      ...(source?.appState || {}),
      exportBackground: true,
      viewBackgroundColor: source?.appState?.viewBackgroundColor || "#ffffff"
    },
    files: source?.files || files
  };
}
