import oc from "open-color";
import type { Merge } from "./utility-types";

// FIXME can't put to utils.ts rn because of circular dependency
const pick = <R extends Record<string, any>, K extends readonly (keyof R)[]>(
  source: R,
  keys: K,
) => {
  return keys.reduce((acc, key: K[number]) => {
    if (key in source) {
      acc[key] = source[key];
    }
    return acc;
  }, {} as Pick<R, K[number]>) as Pick<R, K[number]>;
};

export type ColorPickerColor =
  | Exclude<keyof oc, "indigo" | "lime">
  | "transparent"
  | "bronze";
export type ColorTuple = readonly [string, string, string, string, string];
export type ColorPalette = Merge<
  Record<ColorPickerColor, ColorTuple>,
  { black: "#1e1e1e"; white: "#ffffff"; transparent: "transparent" }
>;

// used general type instead of specific type (ColorPalette) to support custom colors
export type ColorPaletteCustom = { [key: string]: ColorTuple | string };
export type ColorShadesIndexes = [number, number, number, number, number];

export const MAX_CUSTOM_COLORS_USED_IN_CANVAS = 5;
export const COLORS_PER_ROW = 5;

export const DEFAULT_CHART_COLOR_INDEX = 4;

export const DEFAULT_ELEMENT_STROKE_COLOR_INDEX = 4;
export const DEFAULT_ELEMENT_BACKGROUND_COLOR_INDEX = 1;
export const ELEMENTS_PALETTE_SHADE_INDEXES = [0, 2, 4, 6, 8] as const;
export const CANVAS_PALETTE_SHADE_INDEXES = [0, 1, 2, 3, 4] as const;

export const getSpecificColorShades = (
  color: Exclude<
    ColorPickerColor,
    "transparent" | "white" | "black" | "bronze"
  >,
  indexArr: Readonly<ColorShadesIndexes>,
) => {
  return indexArr.map((index) => oc[color][index]) as any as ColorTuple;
};

export const COLOR_PALETTE = {
  transparent: "transparent",
  black: "#1e1e1e",
  white: "#ffffff",
  // open-colors
  gray: getSpecificColorShades("gray", ELEMENTS_PALETTE_SHADE_INDEXES),
  red: getSpecificColorShades("red", ELEMENTS_PALETTE_SHADE_INDEXES),
  pink: getSpecificColorShades("pink", ELEMENTS_PALETTE_SHADE_INDEXES),
  grape: getSpecificColorShades("grape", ELEMENTS_PALETTE_SHADE_INDEXES),
  violet: getSpecificColorShades("violet", ELEMENTS_PALETTE_SHADE_INDEXES),
  blue: getSpecificColorShades("blue", ELEMENTS_PALETTE_SHADE_INDEXES),
  cyan: getSpecificColorShades("cyan", ELEMENTS_PALETTE_SHADE_INDEXES),
  teal: getSpecificColorShades("teal", ELEMENTS_PALETTE_SHADE_INDEXES),
  green: getSpecificColorShades("green", ELEMENTS_PALETTE_SHADE_INDEXES),
  yellow: getSpecificColorShades("yellow", ELEMENTS_PALETTE_SHADE_INDEXES),
  orange: getSpecificColorShades("orange", ELEMENTS_PALETTE_SHADE_INDEXES),
  // radix bronze shades 3,5,7,9,11
  bronze: ["#f8f1ee", "#eaddd7", "#d2bab0", "#a18072", "#846358"],
} as ColorPalette;

const COMMON_ELEMENT_SHADES = pick(COLOR_PALETTE, [
  "cyan",
  "blue",
  "violet",
  "grape",
  "pink",
  "green",
  "teal",
  "yellow",
  "orange",
  "red",
]);

// -----------------------------------------------------------------------------
// quick picks defaults
// -----------------------------------------------------------------------------

// ORDER matters for positioning in quick picker
export const DEFAULT_ELEMENT_STROKE_PICKS = [
  COLOR_PALETTE.black,
  COLOR_PALETTE.red[DEFAULT_ELEMENT_STROKE_COLOR_INDEX],
  COLOR_PALETTE.green[DEFAULT_ELEMENT_STROKE_COLOR_INDEX],
  COLOR_PALETTE.blue[DEFAULT_ELEMENT_STROKE_COLOR_INDEX],
  COLOR_PALETTE.yellow[DEFAULT_ELEMENT_STROKE_COLOR_INDEX],
] as ColorTuple;

// ORDER matters for positioning in quick picker
export const DEFAULT_ELEMENT_BACKGROUND_PICKS = [
  COLOR_PALETTE.transparent,
  COLOR_PALETTE.red[DEFAULT_ELEMENT_BACKGROUND_COLOR_INDEX],
  COLOR_PALETTE.green[DEFAULT_ELEMENT_BACKGROUND_COLOR_INDEX],
  COLOR_PALETTE.blue[DEFAULT_ELEMENT_BACKGROUND_COLOR_INDEX],
  COLOR_PALETTE.yellow[DEFAULT_ELEMENT_BACKGROUND_COLOR_INDEX],
] as ColorTuple;

// ORDER matters for positioning in quick picker
export const DEFAULT_CANVAS_BACKGROUND_PICKS = [
  COLOR_PALETTE.white,
  // radix slate2
  "#f8f9fa",
  // radix blue2
  "#f5faff",
  // radix yellow2
  "#fffce8",
  // radix bronze2
  "#fdf8f6",
] as ColorTuple;

// -----------------------------------------------------------------------------
// palette defaults
// -----------------------------------------------------------------------------

export const DEFAULT_ELEMENT_STROKE_COLOR_PALETTE = {
  // 1st row
  transparent: COLOR_PALETTE.transparent,
  white: COLOR_PALETTE.white,
  gray: COLOR_PALETTE.gray,
  black: COLOR_PALETTE.black,
  bronze: COLOR_PALETTE.bronze,
  // rest
  ...COMMON_ELEMENT_SHADES,
} as const;

// ORDER matters for positioning in pallete (5x3 grid)s
export const DEFAULT_ELEMENT_BACKGROUND_COLOR_PALETTE = {
  transparent: COLOR_PALETTE.transparent,
  white: COLOR_PALETTE.white,
  gray: COLOR_PALETTE.gray,
  black: COLOR_PALETTE.black,
  bronze: COLOR_PALETTE.bronze,

  ...COMMON_ELEMENT_SHADES,
} as const;

// Excalidraw Local switches palettes in-place so already-mounted color pickers
// immediately reflect the selected drawing mode.  The original values are kept
// private and are never written into scene data.
const clonePaletteValue = (value: ColorTuple | string) =>
  Array.isArray(value) ? [...value] : value;
const LOCAL_ORIGINAL_PALETTE = Object.fromEntries(
  Object.entries(COLOR_PALETTE).map(([key, value]) => [
    key,
    clonePaletteValue(value),
  ]),
) as ColorPaletteCustom;
const LOCAL_ORIGINAL_STROKE_PICKS = [...DEFAULT_ELEMENT_STROKE_PICKS];
const LOCAL_ORIGINAL_BACKGROUND_PICKS = [...DEFAULT_ELEMENT_BACKGROUND_PICKS];
const LOCAL_ORIGINAL_CANVAS_PICKS = [...DEFAULT_CANVAS_BACKGROUND_PICKS];
const LOCAL_ORIGINAL_STROKE_PALETTE = {
  ...DEFAULT_ELEMENT_STROKE_COLOR_PALETTE,
};
const LOCAL_ORIGINAL_BACKGROUND_PALETTE = {
  ...DEFAULT_ELEMENT_BACKGROUND_COLOR_PALETTE,
};

// Equivar light figure palette: only the requested seven color families plus
// graphite and warm white are exposed in Professional mode.
const LOCAL_PROFESSIONAL_PALETTE: ColorPaletteCustom = {
  transparent: "transparent",
  black: "#222624", // Graphite
  white: "#FAFAF7", // Warm white
  gray: ["#F0F1F0", "#D6D9D7", "#B6BCB9", "#8B9490", "#222624"],
  bronze: ["#F2EAE5", "#DECAC0", "#C6AA9C", "#A68776", "#6F5747"], // Brown01
  green: ["#E6F0EC", "#CCE0D7", "#A5C9B8", "#77A28B", "#2F6B57"], // Green01
  blue: ["#E4EDF1", "#C8DCE5", "#9FBECD", "#6D97AC", "#356078"], // Blue01
  violet: ["#ECE7F0", "#D8CBE0", "#BCA9C8", "#9179A3", "#66507A"], // Purple01
  yellow: ["#FAF2DC", "#F2DFAD", "#E6C97A", "#D0A84F", "#B38B30"], // Yellow02
  red: ["#F7E9EB", "#EDCFD3", "#DEADB4", "#BE7984", "#994A55"], // Red01
  orange: ["#F8EBE2", "#F0D3BF", "#E4AC88", "#C97F52", "#A95F2B"], // Orange01
};

const replaceObject = (target: Record<string, any>, source: Record<string, any>) => {
  for (const key of Object.keys(target)) {
    delete target[key];
  }
  Object.assign(target, source);
};

export const setLocalProfessionalPalette = (professional: boolean) => {
  const palette = professional
    ? LOCAL_PROFESSIONAL_PALETTE
    : LOCAL_ORIGINAL_PALETTE;

  for (const [key, value] of Object.entries(palette)) {
    const current = (COLOR_PALETTE as any)[key];
    if (Array.isArray(current) && Array.isArray(value)) {
      current.splice(0, current.length, ...value);
    } else {
      (COLOR_PALETTE as any)[key] = clonePaletteValue(value);
    }
  }

  const colors = COLOR_PALETTE as any;
  const paletteRows = professional
    ? {
        transparent: colors.transparent,
        white: colors.white,
        gray: colors.gray,
        black: colors.black,
        bronze: colors.bronze,
        green: colors.green,
        blue: colors.blue,
        violet: colors.violet,
        yellow: colors.yellow,
        red: colors.red,
        orange: colors.orange,
      }
    : LOCAL_ORIGINAL_STROKE_PALETTE;
  replaceObject(DEFAULT_ELEMENT_STROKE_COLOR_PALETTE as any, paletteRows);
  replaceObject(
    DEFAULT_ELEMENT_BACKGROUND_COLOR_PALETTE as any,
    professional ? paletteRows : LOCAL_ORIGINAL_BACKGROUND_PALETTE,
  );

  const strokePicks = professional
    ? [colors.black, colors.red[4], colors.green[4], colors.blue[4], colors.yellow[4]]
    : LOCAL_ORIGINAL_STROKE_PICKS;
  DEFAULT_ELEMENT_STROKE_PICKS.splice(0, DEFAULT_ELEMENT_STROKE_PICKS.length, ...strokePicks);

  const backgroundPicks = professional
    ? [colors.transparent, colors.red[0], colors.green[0], colors.blue[0], colors.yellow[0]]
    : LOCAL_ORIGINAL_BACKGROUND_PICKS;
  DEFAULT_ELEMENT_BACKGROUND_PICKS.splice(0, DEFAULT_ELEMENT_BACKGROUND_PICKS.length, ...backgroundPicks);

  const canvasPicks = professional
    ? [colors.white, colors.gray[0], colors.blue[0], colors.yellow[0], colors.bronze[0]]
    : LOCAL_ORIGINAL_CANVAS_PICKS;
  DEFAULT_CANVAS_BACKGROUND_PICKS.splice(0, DEFAULT_CANVAS_BACKGROUND_PICKS.length, ...canvasPicks);
};

// -----------------------------------------------------------------------------
// helpers
// -----------------------------------------------------------------------------

// !!!MUST BE WITHOUT GRAY, TRANSPARENT AND BLACK!!!
export const getAllColorsSpecificShade = (index: 0 | 1 | 2 | 3 | 4) =>
  [
    // 2nd row
    COLOR_PALETTE.cyan[index],
    COLOR_PALETTE.blue[index],
    COLOR_PALETTE.violet[index],
    COLOR_PALETTE.grape[index],
    COLOR_PALETTE.pink[index],

    // 3rd row
    COLOR_PALETTE.green[index],
    COLOR_PALETTE.teal[index],
    COLOR_PALETTE.yellow[index],
    COLOR_PALETTE.orange[index],
    COLOR_PALETTE.red[index],
  ] as const;

export const rgbToHex = (r: number, g: number, b: number) =>
  `#${((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1)}`;

// -----------------------------------------------------------------------------
