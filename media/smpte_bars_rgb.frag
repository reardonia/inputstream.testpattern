// SMPTE 100% color bars in RGB, output to a DRM_FORMAT_XRGB* DMA-BUF.
//
// Byte order note: DRM_FORMAT_XRGB8888 expects memory bytes B,G,R,X.
// OpenGL writes channel 0 (R) to memory byte 0, channel 1 (G) to byte 1, etc.
// To make the display interpret colors correctly we swap R<->B in the shader:
// what we call "red" is written to gl_FragColor.b (which goes to memory byte 2),
// and "blue" is written to gl_FragColor.r (memory byte 0). The display then
// reads memory byte 0 as B, byte 2 as R, and the colors come out right.
//
// For 10-bit (DRM_FORMAT_XRGB2101010) and 16-bit (DRM_FORMAT_XRGB16161616 /
// DRM_FORMAT_XRGB16161616F) the same R/B swap applies because the in-memory
// dword layout is X:R:G:B (little-endian).

#ifdef GL_ES
precision mediump float;
#endif

uniform vec2 u_resolution;
uniform float u_time;
uniform int u_frame;
uniform vec2 u_frameCountPos; // top-left of counter box in video pixel coords; (-1,-1) disabled

// All colors below are 8-bit limited-range RGB values (16 = reference black,
// 235 = reference white, 0..15 is footroom, 236..255 is headroom). Divided
// by 255 at the end to produce normalized [0,1] shader output. The pattern
// file declares range=limited so downstream consumers treat these correctly.
const float BLACK = 16.0 / 255.0;
const float WHITE = 235.0 / 255.0;

// 7 color bars at 100% amplitude in limited-range RGB:
//   white (235,235,235), yellow (235,235,16), cyan (16,235,235),
//   green (16,235,16), magenta (235,16,235), red (235,16,16), blue (16,16,235)
vec3 colorBarRgb(float x)
{
  if (x < 1.0 / 7.0)      return vec3(WHITE, WHITE, WHITE); // white
  else if (x < 2.0 / 7.0) return vec3(WHITE, WHITE, BLACK); // yellow
  else if (x < 3.0 / 7.0) return vec3(BLACK, WHITE, WHITE); // cyan
  else if (x < 4.0 / 7.0) return vec3(BLACK, WHITE, BLACK); // green
  else if (x < 5.0 / 7.0) return vec3(WHITE, BLACK, WHITE); // magenta
  else if (x < 6.0 / 7.0) return vec3(WHITE, BLACK, BLACK); // red
  else                    return vec3(BLACK, BLACK, WHITE); // blue
}

// Castellation strip: blue, black, magenta, black, cyan, black, white
// (reverse blue pattern for chrominance alignment).
vec3 castellationsRgb(float x)
{
  if (x < 1.0 / 7.0)      return vec3(BLACK, BLACK, WHITE); // blue
  else if (x < 2.0 / 7.0) return vec3(BLACK, BLACK, BLACK); // black
  else if (x < 3.0 / 7.0) return vec3(WHITE, BLACK, WHITE); // magenta
  else if (x < 4.0 / 7.0) return vec3(BLACK, BLACK, BLACK); // black
  else if (x < 5.0 / 7.0) return vec3(BLACK, WHITE, WHITE); // cyan
  else if (x < 6.0 / 7.0) return vec3(BLACK, BLACK, BLACK); // black
  else                    return vec3(WHITE, WHITE, WHITE); // white
}

// Bottom 1/4 area, SMPTE ECR 1-1978 layout.
// Six boxes across the width, split 4+2:
//   Boxes 1-4 cover the left 5/7 of width (each 5/28, aligning with the
//   first 5 color bars above). Boxes 5-6 cover the right 2/7 of width
//   (each 4/28 = 1/7, aligning with the last 2 color bars).
//
//   1: -I signal       (dark blue-purple)
//   2: 100% white
//   3: +Q signal       (purple)
//   4: black           (reference)
//   5: PLUGE stripes   (sub-black, reference black, super-black)
//   6: black           (right edge)
//
// Limited-range RGB makes proper PLUGE possible: reference black is at
// Y=16/255 and we can encode values below that (down to 0) as sub-black
// footroom. A calibrated display should:
//   - Hide the sub-black stripe (Y=8)   -- blends with reference black
//   - Show reference black stripe (Y=16) as the deepest visible black
//   - Barely show the super-black stripe (Y=24) as just above reference
vec3 plugeAreaRgb(float x)
{
  // 28 cells total. Boxes 1-4 are 5 cells wide each, boxes 5-6 are 4 each.
  float cell = x * 28.0;
  if (cell < 5.0)       return vec3( 34.0,  47.0,  90.0) / 255.0; // -I
  else if (cell < 10.0) return vec3(WHITE, WHITE, WHITE);         // 100% white
  else if (cell < 15.0) return vec3( 80.0,  35.0, 112.0) / 255.0; // +Q
  else if (cell < 20.0) return vec3(BLACK, BLACK, BLACK);         // black
  else if (cell < 24.0)
  {
    // 3 narrow PLUGE stripes within box 5 (4 cells wide): sub-black,
    // reference black, super-black. Each stripe = 4/3 cells wide.
    float pcell = (cell - 20.0) * 3.0 / 4.0; // 0..3
    if (pcell < 1.0)      return vec3(8.0 / 255.0);   // Y=8  (below ref)
    else if (pcell < 2.0) return vec3(16.0 / 255.0);  // Y=16 (reference)
    else                  return vec3(24.0 / 255.0);  // Y=24 (above ref)
  }
  return vec3(BLACK, BLACK, BLACK); // box 6: right-edge black
}

// 7-segment digit rendering, copied from the YUV shader.
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

void main()
{
  vec2 frag = gl_FragCoord.xy;
  float vx = frag.x / u_resolution.x;
  // Render directly into the scanout DMA-BUF: row 0 is the top of the display,
  // which in OpenGL fragment coordinates is the TOP of the framebuffer
  // (frag.y = u_resolution.y - 1). So "video y" increases as frag.y decreases.
  // No flip needed -- gl_FragCoord.y naturally decreases toward the bottom
  // of the video when reading row-by-row from the scanout side.
  float vy = frag.y / u_resolution.y;

  vec3 rgb;
  if (vy < 2.0 / 3.0)
    rgb = colorBarRgb(vx);
  else if (vy < 3.0 / 4.0)
    rgb = castellationsRgb(vx);
  else
    rgb = plugeAreaRgb(vx);

  // Frame counter overlay
  if (u_frameCountPos.x >= 0.0)
  {
    float scale = 4.0;
    float digitW = 10.0 * scale;
    float digitH = 14.0 * scale;
    float space = 2.0 * scale;
    float pad = space;
    float boxW = 4.0 * digitW + 3.0 * space + 2.0 * pad;
    float boxH = digitH + 2.0 * pad;

    // u_frameCountPos is in video coords (y=0 at top). Since we're rendering
    // directly into the scanout buffer (row 0 = top), video y matches frag.y
    // when counting from the bottom of the framebuffer, but the user specified
    // from the top. Convert: top-local = u_frameCountPos.y, then shader_y =
    // u_resolution.y - u_frameCountPos.y - boxH. For localY (top-down in the
    // box) we invert:
    float localX = frag.x - u_frameCountPos.x;
    float localY = frag.y - u_frameCountPos.y;

    if (localX >= 0.0 && localX < boxW && localY >= 0.0 && localY < boxH)
    {
      rgb = vec3(BLACK, BLACK, BLACK); // reference-black background

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
            rgb = vec3(WHITE, WHITE, WHITE);
        }
      }
    }
  }

  // The GL driver handles DRM_FORMAT_XRGB8888 memory layout automatically
  // for EGLImage-imported DMA-BUFs: fragColor.r is the logical red channel,
  // regardless of whether the underlying memory is stored as B,G,R,X or
  // R,G,B,X. No explicit byte-order swap is needed in the shader.
  gl_FragColor = vec4(rgb, 1.0);
}
