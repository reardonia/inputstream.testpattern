// SMPTE RP 219-1:2014 HDTV color bars, rendered directly into a DRM_FORMAT_XRGB*
// DMA-BUF.
//
// Normative references:
//
//   [RP219-1] SMPTE RP 219-1:2014, "High-Definition, Standard-Definition
//             Compatible Color Bar Signal." Defines the 1920x1080 HDTV color
//             bar layout and the optional -I / +Q sub-patterns.
//
//   [RP219-2] SMPTE RP 219-2:2016, "Ultra High-Definition, 2048 x 1080 and
//             4096 x 2160 Compatible Color Bar Signal." Supersedes RP 219-1
//             for UHDTV. Preserves the RP 219-1 structure but drops the
//             -I / +Q options.
//             * Figure 1            -- overall pattern layout (4 patterns,
//                                      side panels, 4:3 center area).
//             * Table B.1           -- Pattern 1 10-bit Y/Cb/Cr for 75% bars
//                                      and 40% gray, BT.709 colorimetry.
//             * Table B.2, B.3      -- Pattern 2 Y/Cb/Cr (75%/100% white).
//             * Table B.4           -- Pattern 3 Y/Cb/Cr (0% black, ramp).
//             * Table B.5           -- Pattern 4 Y/Cb/Cr (15% gray, 100%
//                                      white, 0% black, -2/+2/+4% PLUGE,
//                                      sub-black valley, super-white peak).
//             * Tables C.1-C.7      -- per-format integer bar widths
//                                      (k/g/h/i/j/m) for 1080p/UHD1/UHD2.
//             * Table C.8           -- pattern heights (7/12, 1/12, 1/12,
//                                      3/12 of active height b).
//
//   [BT.709]  ITU-R BT.709-6 (2015), "Parameter values for the HDTV standards
//             for production and international programme exchange." Defines
//             the BT.709 primaries, transfer function, and YCbCr matrix used
//             by RP 219 "conventional colorimetry."
//
// Geometry and Y/Cb/Cr reference values are taken from RP 219-2, which
// preserves the RP 219-1 structure. Codes are converted to limited-range RGB
// for shader output (the rendering pipeline downstream interprets the output
// as limited-range RGB in a BT.709 container). The -I and +Q sub-patterns
// (Pattern 2 *2 and Pattern 3 *3 in Figure 1), which RP 219-1 permits and
// RP 219-2 explicitly drops, are kept behind compile-time flags so the same
// shader can produce either variant. Authoritative 10-bit Y/Cb/Cr for -I and
// +Q are in RP 219-1 Annex B; the approximate values here were derived from
// the Wikipedia SMPTE_Color_Bars_16x9.svg sRGB swatches (#003f69, #410077)
// because RP 219-1 is behind a paywall and was not consulted directly.
//
// Output convention (same as smpte_bars_rgb.frag):
//   - Limited-range RGB: 16/255 = reference black, 235/255 = reference white.
//   - gl_FragColor written directly. The GL driver maps logical R/G/B to the
//     DMA-BUF byte order for DRM_FORMAT_XRGB* automatically for EGLImage
//     imports, so no R<->B swap is needed.
//   - u_resolution is the target resolution. The layout is resolution-agnostic
//     (fractional coordinates); geometry matches SMPTE RP 219 regardless of
//     the actual DMA-BUF size, as long as aspect ratio is 16:9.
//   - Frame counter overlay logic copied verbatim from smpte_bars_rgb.frag.

#ifdef GL_ES
precision mediump float;
#endif

uniform vec2 u_resolution;
uniform float u_time;
uniform int u_frame;
uniform vec2 u_frameCountPos; // top-left of counter box in video pixel coords; (-1,-1) disabled

// RP 219-1 optional sub-patterns. Set both to 0 for pure RP 219-2 (UHDTV)
// layout, both to 1 for the classic HDTV pattern shown on Wikipedia.
#define USE_MINUS_I 1 // *2 sub-pattern: 0 = 75% white, 1 = -I signal
#define USE_PLUS_Q  1 // *3 sub-pattern: 0 =  0% black, 1 = +Q signal

// Side-panel fraction d/a. For 16:9 formats (1920x1080, 3840x2160, 7680x4320)
// this is exactly 1/8. For 256:135 formats (2048x1080, 4096x2160) it would be
// slightly different; those formats are out of scope here.
const float D_FRAC = 1.0 / 8.0;

// ============================================================================
// Reference levels (limited range, BT.709)
// ============================================================================
// Why we carry two separate sets of constants, one per bit depth:
//
// SMPTE RP 219-2 Annex B specifies the reference Y (and Cb,Cr) codes as
// integers at both 10-bit and 12-bit. The 8-bit codes are conventionally
// derived from the same underlying percentages (e.g. "40% gray" is
// 0.40 * 219 + 16 rounded to an integer). Crucially, each bit depth's code is
// rounded INDEPENDENTLY from the continuous percentage -- the 10-bit code is
// NOT defined as "the 8-bit code times 4" and vice versa.
//
// A shader that picks one set of constants and relies on the GPU's
// framebuffer quantization (round(float * max_value)) to produce the other
// does not reach exact spec compliance at both depths. There are two reasons:
//
//   1. The float-to-int scale factor between 8-bit and 10-bit outputs is
//      1023/255 = 4.0118, not 4.0. So (V10 / 1023.0) * 255 != V10 / 4 for
//      most values, and the round() step lands on a different integer.
//
//   2. Because the 8-bit and 10-bit spec codes are each rounded independently
//      from the percentage, two codes that both round from the same real
//      number can end up on opposite sides of a half-step. Re-quantizing one
//      into the other via GPU float math then rounds the "wrong" way for some
//      values.
//
// Concrete mismatches that fall out of using 10-bit constants / 1023 for an
// 8-bit framebuffer:
//   * 40% gray:  10-bit 414 / 1023 * 255 = 103.2 → 103, but 8-bit spec = 104
//   * 100% white:10-bit 940 / 1023 * 255 = 234.3 → 234, but 8-bit spec = 235
//   * -2% PLUGE: 10-bit  46 / 1023 * 255 =  11.5 → 11,  but 8-bit spec = 12
//
// Each is only 1 code off, but these are the exact values calibrators will
// probe with a waveform monitor or pixel picker. A test pattern whose codes
// do not match the spec at bit-depth granularity defeats its own purpose.
//
// Solution: compile-time OUTPUT_10BIT selector. Set =1 for 10-bit output
// formats (XRGB2101010, XRGB16161616), =0 for XRGB8888. Each branch uses the
// exact Annex B (or equivalent) integer code for that bit depth.
#ifndef OUTPUT_10BIT
#define OUTPUT_10BIT 1
#endif

#if OUTPUT_10BIT
// 10-bit codes from Annex B (conventional colorimetry). Max code = 1023.
const float DENOM      = 1023.0;
const float BLACK      =   64.0 / DENOM; // 0% black, reference
const float WHITE      =  940.0 / DENOM; // 100% white, reference
const float GRAY_75    =  721.0 / DENOM; // 75% white/gray (Pattern 1 bars)
const float GRAY_40    =  414.0 / DENOM; // 40% gray (Pattern 1 side panels)
const float GRAY_15    =  195.0 / DENOM; // 15% gray (Pattern 4 side panels)
// Pattern 4 PLUGE, Table B.5
const float PLUGE_NEG2 =   46.0 / DENOM; // -2% sub-black
const float PLUGE_REF  =   64.0 / DENOM; //  0% reference black
const float PLUGE_POS2 =   82.0 / DENOM; // +2% black
const float PLUGE_POS4 =   99.0 / DENOM; // +4% black
// RP 219-1 optional sub-patterns (-I, +Q). Annex B of RP 219-2 does not list
// these (they were dropped in RP 219-2). Values derived from the Wikipedia
// SMPTE_Color_Bars_16x9.svg full-range sRGB swatches (#003f69, #410077)
// mapped to limited-range 10-bit via V10 = V_full * 876 + 64.
const vec3  MINUS_I    = vec3( 64.0, 280.0, 425.0) / DENOM;
const vec3  PLUS_Q     = vec3(287.0,  64.0, 472.0) / DENOM;
#else
// 8-bit codes. Max code = 255.
const float DENOM      =  255.0;
const float BLACK      =   16.0 / DENOM;
const float WHITE      =  235.0 / DENOM;
const float GRAY_75    =  180.0 / DENOM; // 75% of 219 + 16
const float GRAY_40    =  104.0 / DENOM; // 40% of 219 + 16 = 103.6 → 104
const float GRAY_15    =   49.0 / DENOM; // 15% of 219 + 16 =  48.85 → 49
const float PLUGE_NEG2 =   12.0 / DENOM; // 16 - 4.38 → 12
const float PLUGE_REF  =   16.0 / DENOM;
const float PLUGE_POS2 =   20.0 / DENOM; // 16 + 4.38 → 20
const float PLUGE_POS4 =   25.0 / DENOM; // 16 + 8.76 → 25
const vec3  MINUS_I    = vec3( 16.0,  70.0, 106.0) / DENOM;
const vec3  PLUS_Q     = vec3( 72.0,  16.0, 118.0) / DENOM;
#endif

// ============================================================================
// 7-segment digit rendering (copied from smpte_bars_rgb.frag)
// ============================================================================
int segmentMask(int d)
{
  if (d == 0) return 63;
  else if (d == 1) return 6;
  else if (d == 2) return 91;
  else if (d == 3) return 79;
  else if (d == 4) return 102;
  else if (d == 5) return 109;
  else if (d == 6) return 125;
  else if (d == 7) return 7;
  else if (d == 8) return 127;
  else if (d == 9) return 111;
  return 0;
}

bool segOn(int mask, int seg)
{
  float p = pow(2.0, float(seg));
  return mod(floor(float(mask) / p), 2.0) >= 1.0;
}

bool digitPixel(int d, float x, float y)
{
  int mask = segmentMask(d);
  if (y >= 0.0 && y < 2.0 && x >= 1.0 && x < 9.0 && segOn(mask, 0)) return true;
  if (y >= 1.0 && y < 7.0 && x >= 8.0 && x < 10.0 && segOn(mask, 1)) return true;
  if (y >= 7.0 && y < 13.0 && x >= 8.0 && x < 10.0 && segOn(mask, 2)) return true;
  if (y >= 12.0 && y < 14.0 && x >= 1.0 && x < 9.0 && segOn(mask, 3)) return true;
  if (y >= 7.0 && y < 13.0 && x >= 0.0 && x < 2.0 && segOn(mask, 4)) return true;
  if (y >= 1.0 && y < 7.0 && x >= 0.0 && x < 2.0 && segOn(mask, 5)) return true;
  if (y >= 6.0 && y < 8.0 && x >= 1.0 && x < 9.0 && segOn(mask, 6)) return true;
  return false;
}

// ============================================================================
// Per-pattern color lookup. x_43 is the normalized horizontal position within
// the central 4:3 area (0 at left edge, 1 at right edge of the 4:3 region).
// ============================================================================

// Pattern 1: 7 color bars at 75% amplitude, in the standard
// W, Y, C, G, M, R, B order (descending Y). Each bar is c wide (1/7 of 4:3).
vec3 pattern1(float x_43)
{
  int bar = int(floor(x_43 * 7.0));
  if      (bar == 0) return vec3(GRAY_75, GRAY_75, GRAY_75); // 75% white
  else if (bar == 1) return vec3(GRAY_75, GRAY_75, BLACK);   // 75% yellow
  else if (bar == 2) return vec3(BLACK,   GRAY_75, GRAY_75); // 75% cyan
  else if (bar == 3) return vec3(BLACK,   GRAY_75, BLACK);   // 75% green
  else if (bar == 4) return vec3(GRAY_75, BLACK,   GRAY_75); // 75% magenta
  else if (bar == 5) return vec3(GRAY_75, BLACK,   BLACK);   // 75% red
  else               return vec3(BLACK,   BLACK,   GRAY_75); // 75% blue
}

// Pattern 2: chroma-setting reference row.
//   *2 sub-pattern (first c) = -I (RP 219-1) or 75% white (RP 219-2)
//   remainder (6c) = 75% white
vec3 pattern2(float x_43)
{
#if USE_MINUS_I
  if (x_43 < 1.0 / 7.0) return MINUS_I;
#endif
  return vec3(GRAY_75, GRAY_75, GRAY_75); // 75% white
}

// Pattern 3: Y-ramp row.
//   *3 sub-pattern (first c)   = +Q (RP 219-1) or 0% black (RP 219-2)
//   middle (5c)                = luminance ramp 0% → 100%
//   rightmost c                = 100% white
vec3 pattern3(float x_43)
{
  if (x_43 < 1.0 / 7.0)
  {
#if USE_PLUS_Q
    return PLUS_Q;
#else
    return vec3(BLACK);
#endif
  }
  else if (x_43 < 6.0 / 7.0)
  {
    // Linear Y ramp across 5c (middle of the 4:3 area)
    float t = (x_43 - 1.0 / 7.0) / (5.0 / 7.0);
    return vec3(BLACK + t * (WHITE - BLACK));
  }
  return vec3(WHITE); // rightmost c: 100% white
}

// Pattern 4: PLUGE row. Widths (from RP 219-2 Annex C Table C.2, inside the
// 4:3 area of width 7c):
//   k = 0% black       = 1.5c
//   g = 100% white     = 2c
//   h = 0% black       = 5c/6
//   i = -2 / 0 / +2    = c/3 each (3 strips)
//   j =  0 / +4        = c/3 each (2 strips)
//   m = 0% black       = c
// Total = 1.5 + 2 + 5/6 + 1 + 2/3 + 1 = 7c ✓
//
// Work in 42nds of the 4:3 area (= 1/6 c), so boundaries are integers:
//   k: 0..9, g: 9..21, h: 21..26, i1: 26..28, i2: 28..30, i3: 30..32,
//   j1: 32..34, j2: 34..36, m: 36..42
vec3 pattern4(float x_43)
{
  float cell = x_43 * 42.0;
  if      (cell <  9.0) return vec3(BLACK);      // k: 0% black
  else if (cell < 21.0) return vec3(WHITE);      // g: 100% white
  else if (cell < 26.0) return vec3(BLACK);      // h: 0% black
  else if (cell < 28.0) return vec3(PLUGE_NEG2); // i1: -2% sub-black
  else if (cell < 30.0) return vec3(PLUGE_REF);  // i2: reference black
  else if (cell < 32.0) return vec3(PLUGE_POS2); // i3: +2% black
  else if (cell < 34.0) return vec3(PLUGE_REF);  // j1: reference black
  else if (cell < 36.0) return vec3(PLUGE_POS4); // j2: +4% black
  return vec3(BLACK);                            // m: 0% black
}

// ============================================================================
// Main
// ============================================================================
void main()
{
  vec2 frag = gl_FragCoord.xy;
  float vx = frag.x / u_resolution.x;
  // Row 0 is the top of the display; fragment coords naturally decrease as we
  // move down the video, which matches when rendering directly into the
  // scanout buffer (no flip needed).
  float vy = frag.y / u_resolution.y;

  // Pattern heights (Table C.8): 7/12, 1/12, 1/12, 3/12
  //   Pattern 1: y in [0,      7/12)
  //   Pattern 2: y in [7/12,   8/12)
  //   Pattern 3: y in [8/12,   9/12)
  //   Pattern 4: y in [9/12,  12/12)

  vec3 rgb;

  if (vy < 7.0 / 12.0)
  {
    // Pattern 1: 40% gray (d) | 7 color bars (4:3 area) | 40% gray (d)
    if (vx < D_FRAC || vx >= 1.0 - D_FRAC)
      rgb = vec3(GRAY_40);
    else
      rgb = pattern1((vx - D_FRAC) / (1.0 - 2.0 * D_FRAC));
  }
  else if (vy < 8.0 / 12.0)
  {
    // Pattern 2: 100% cyan (d) | 75% white / -I (4:3) | 100% blue (d)
    if (vx < D_FRAC)
      rgb = vec3(BLACK, WHITE, WHITE); // 100% cyan
    else if (vx >= 1.0 - D_FRAC)
      rgb = vec3(BLACK, BLACK, WHITE); // 100% blue
    else
      rgb = pattern2((vx - D_FRAC) / (1.0 - 2.0 * D_FRAC));
  }
  else if (vy < 9.0 / 12.0)
  {
    // Pattern 3: 100% yellow (d) | +Q / ramp / 100% white (4:3) | 100% red (d)
    if (vx < D_FRAC)
      rgb = vec3(WHITE, WHITE, BLACK); // 100% yellow
    else if (vx >= 1.0 - D_FRAC)
      rgb = vec3(WHITE, BLACK, BLACK); // 100% red
    else
      rgb = pattern3((vx - D_FRAC) / (1.0 - 2.0 * D_FRAC));
  }
  else
  {
    // Pattern 4: 15% gray (d) | black / white / PLUGE (4:3) | 15% gray (d)
    if (vx < D_FRAC || vx >= 1.0 - D_FRAC)
      rgb = vec3(GRAY_15);
    else
      rgb = pattern4((vx - D_FRAC) / (1.0 - 2.0 * D_FRAC));
  }

  // ==========================================================================
  // Frame counter overlay (copied from smpte_bars_rgb.frag)
  // ==========================================================================
  if (u_frameCountPos.x >= 0.0)
  {
    float scale = 4.0;
    float digitW = 10.0 * scale;
    float digitH = 14.0 * scale;
    float space = 2.0 * scale;
    float pad = space;
    float boxW = 4.0 * digitW + 3.0 * space + 2.0 * pad;
    float boxH = digitH + 2.0 * pad;

    float localX = frag.x - u_frameCountPos.x;
    float localY = frag.y - u_frameCountPos.y;

    if (localX >= 0.0 && localX < boxW && localY >= 0.0 && localY < boxH)
    {
      rgb = vec3(BLACK); // reference-black background

      float innerX = localX - pad;
      float innerY = localY - pad;
      if (innerX >= 0.0 && innerY >= 0.0 && innerY < digitH)
      {
        float cellW = digitW + space;
        int digitIdx = int(floor(innerX / cellW));
        float cellX = innerX - float(digitIdx) * cellW;
        if (digitIdx >= 0 && digitIdx < 4 && cellX < digitW)
        {
          float baseX = cellX / scale;
          float baseY = innerY / scale;

          int divisor = 1;
          if (digitIdx == 0) divisor = 1000;
          else if (digitIdx == 1) divisor = 100;
          else if (digitIdx == 2) divisor = 10;
          int d = int(mod(float(u_frame / divisor), 10.0));

          if (digitPixel(d, baseX, baseY))
            rgb = vec3(WHITE);
        }
      }
    }
  }

  gl_FragColor = vec4(rgb, 1.0);
}
