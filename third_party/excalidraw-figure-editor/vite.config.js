import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

function addLocalFonts() {
  return {
    name: "add-local-fonts",
    transform(code, id) {
      if (!id.endsWith("@excalidraw/excalidraw/dist/prod/chunk-K2UTITRG.js") || code.includes("Verdana:10")) return null;
      const familyMarker = 'Liberation Sans":9},Un=';
      const initializationMarker = 'n("Liberation Sans",...lc),n("Lilita One"';
      if (!code.includes(familyMarker) || !code.includes(initializationMarker)) {
        throw new Error("Unsupported Excalidraw font bundle; update the local-font Vite transform.");
      }
      return code
        .replace(familyMarker, 'Liberation Sans":9,Verdana:10,Ubuntu:11},Un=')
        .replace(initializationMarker, 'n("Liberation Sans",...lc),n("Verdana",{uri:"local:Verdana"}),n("Ubuntu",{uri:"./fonts/Ubuntu/Ubuntu-R.ttf"}),n("Lilita One"');
    }
  };
}

function addProfessionalPalette() {
  return {
    name: "add-professional-palette",
    transform(code, id) {
      if (!id.endsWith("@excalidraw/excalidraw/dist/prod/chunk-K2UTITRG.js") || code.includes("EXCALIDRAW_PROFESSIONAL_MODE")) return null;
      const marker = ',j9={transparent:Z.transparent';
      if (!code.includes(marker)) throw new Error("Unsupported Excalidraw color bundle; update the professional-palette Vite transform.");
      const professionalPalette = `;const EXCALIDRAW_LOCAL_ORIGINAL_PALETTE={black:Z.black,white:Z.white,gray:[...Z.gray],bronze:[...Z.bronze],green:[...Z.green],blue:[...Z.blue],violet:[...Z.violet],yellow:[...Z.yellow],red:[...Z.red],orange:[...Z.orange],ns:{...Ns},stroke:[...W9],background:[...z9],canvas:[...Q9]},EXCALIDRAW_LOCAL_PROFESSIONAL_PALETTE={black:"#222624",white:"#FAFAF7",gray:["#F0F1F0","#D6D9D7","#B6BCB9","#8B9490","#222624"],bronze:["#F2EAE5","#DECAC0","#C6AA9C","#A68776","#6F5747"],green:["#E6F0EC","#CCE0D7","#A5C9B8","#77A28B","#2F6B57"],blue:["#E4EDF1","#C8DCE5","#9FBECD","#6D97AC","#356078"],violet:["#ECE7F0","#D8CBE0","#BCA9C8","#9179A3","#66507A"],yellow:["#FAF2DC","#F2DFAD","#E6C97A","#D0A84F","#B38B30"],red:["#F7E9EB","#EDCFD3","#DEADB4","#BE7984","#994A55"],orange:["#F8EBE2","#F0D3BF","#E4AC88","#C97F52","#A95F2B"],ns:{bronze:Z.bronze,blue:Z.blue,violet:Z.violet,green:Z.green,yellow:Z.yellow,orange:Z.orange,red:Z.red},stroke:["#222624","#994A55","#2F6B57","#356078","#B38B30"],background:[Z.transparent,"#F7E9EB","#E6F0EC","#E4EDF1","#FAF2DC"],canvas:["#FAFAF7","#F0F1F0","#E4EDF1","#FAF2DC","#F2EAE5"]};window.EXCALIDRAW_LOCAL_SET_PALETTE=e=>{const t=e?EXCALIDRAW_LOCAL_PROFESSIONAL_PALETTE:EXCALIDRAW_LOCAL_ORIGINAL_PALETTE;Z.black=t.black,Z.white=t.white,Z.gray.splice(0,Z.gray.length,...t.gray),Z.bronze.splice(0,Z.bronze.length,...t.bronze),Z.green.splice(0,Z.green.length,...t.green),Z.blue.splice(0,Z.blue.length,...t.blue),Z.violet.splice(0,Z.violet.length,...t.violet),Z.yellow.splice(0,Z.yellow.length,...t.yellow),Z.red.splice(0,Z.red.length,...t.red),Z.orange.splice(0,Z.orange.length,...t.orange),Object.keys(Ns).forEach(e=>delete Ns[e]),Object.assign(Ns,t.ns),W9.splice(0,W9.length,...t.stroke),z9.splice(0,z9.length,...t.background),Q9.splice(0,Q9.length,...t.canvas);const n={transparent:Z.transparent,white:Z.white,gray:Z.gray,black:Z.black,bronze:Z.bronze,...Ns};j9&&(Object.keys(j9).forEach(e=>delete j9[e]),Object.assign(j9,n)),eE&&(Object.keys(eE).forEach(e=>delete eE[e]),Object.assign(eE,n))},window.EXCALIDRAW_LOCAL_SET_PALETTE(Boolean(window.EXCALIDRAW_PROFESSIONAL_MODE));var `;
      return code.replace(marker, `${professionalPalette}${marker.slice(1)}`);
    }
  };
}

export default defineConfig({
  base: "./",
  plugins: [react(), addLocalFonts(), addProfessionalPalette()],
  build: { outDir: "dist", emptyOutDir: true }
});
