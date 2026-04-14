// SMPTE 100% color bars in BT.2020 limited range YCbCr, with optional frame counter.
// Output: gl_FragColor.rgb = vec3(Y, Cb, Cr) normalized to [0,1]
// Values computed from BT.2020 matrix coefficients (Kr=0.2627, Kb=0.0593)
// Limited range: Y=[16,235], Cb/Cr=[16,240] for 8-bit
//
// Layout (SMPTE-ish):
//   Top 2/3:     7 color bars at 100% amplitude
//   Middle 1/12: reverse castellations (blue, black, magenta, black, cyan, black, white)
//   Bottom 1/4:  -I, white, +Q, black, pluge (-4%/0%/+4%), black
//
// Frame counter: enabled when u_frameCountPos.x >= 0; top-left of box at that pixel
// (top-down video coords). 4 decimal digits rendered as 7-segment displays.

#ifdef GL_ES
precision mediump float;
#endif

uniform vec2 u_resolution;
uniform float u_time;
uniform int u_frame;
uniform vec2 u_frameCountPos; // top-left of counter box in video pixel coords; (-1,-1) disabled

// BT.2020 limited-range 8-bit YCbCr values for 100% color bars.
vec3 colorBar8bit(float x)
{
  if (x < 1.0 / 7.0)      return vec3(235.0, 128.0, 128.0); // white
  else if (x < 2.0 / 7.0) return vec3(222.0,  16.0, 137.0); // yellow
  else if (x < 3.0 / 7.0) return vec3(177.0, 159.0,  16.0); // cyan
  else if (x < 4.0 / 7.0) return vec3(164.0,  47.0,  25.0); // green
  else if (x < 5.0 / 7.0) return vec3( 87.0, 209.0, 231.0); // magenta
  else if (x < 6.0 / 7.0) return vec3( 74.0,  97.0, 240.0); // red
  else                    return vec3( 29.0, 240.0, 119.0); // blue
}

// Reverse castellations strip (for chrominance alignment).
vec3 castellations8bit(float x)
{
  if (x < 1.0 / 7.0)      return vec3( 29.0, 240.0, 119.0); // blue
  else if (x < 2.0 / 7.0) return vec3( 16.0, 128.0, 128.0); // black
  else if (x < 3.0 / 7.0) return vec3( 87.0, 209.0, 231.0); // magenta
  else if (x < 4.0 / 7.0) return vec3( 16.0, 128.0, 128.0); // black
  else if (x < 5.0 / 7.0) return vec3(177.0, 159.0,  16.0); // cyan
  else if (x < 6.0 / 7.0) return vec3( 16.0, 128.0, 128.0); // black
  else                    return vec3(235.0, 128.0, 128.0); // white
}

// Bottom 1/4 area, SMPTE ECR 1-1978 layout.
// Six boxes across the width, split 4+2:
//   Boxes 1-4 cover the left 5/7 of width (each 5/28, aligning with the
//   first 5 color bars). Boxes 5-6 cover the right 2/7 (each 4/28 = 1/7).
//   1: -I       2: 100% white       3: +Q       4: black
//   5: PLUGE    6: black
// -I and +Q are the NTSC quadrature axis signals (approximated in BT.2020 YCbCr).
vec3 plugeArea8bit(float x)
{
  float cell = x * 28.0;
  if (cell < 5.0)       return vec3( 16.0, 198.0,  78.0); // -I (approx)
  else if (cell < 10.0) return vec3(235.0, 128.0, 128.0); // 100% white
  else if (cell < 15.0) return vec3( 38.0, 162.0, 195.0); // +Q (approx)
  else if (cell < 20.0) return vec3( 16.0, 128.0, 128.0); // black
  else if (cell < 24.0)
  {
    // 3 narrow PLUGE stripes within box 5: sub-black, reference, super-black
    float pcell = (cell - 20.0) * 3.0 / 4.0; // 0..3
    if (pcell < 1.0)      return vec3(  8.0, 128.0, 128.0); // -4% sub-black
    else if (pcell < 2.0) return vec3( 16.0, 128.0, 128.0); // reference black
    else                  return vec3( 24.0, 128.0, 128.0); // +4% super-black
  }
  return vec3(16.0, 128.0, 128.0); // box 6: right-edge black
}

// 7-segment display encoding for digits 0-9.
// Bits (LSB->MSB): a=top, b=topR, c=botR, d=bot, e=botL, f=topL, g=mid
int segmentMask(int d)
{
  if (d == 0) return 63;   // 0111111
  else if (d == 1) return 6;    // 0000110
  else if (d == 2) return 91;   // 1011011
  else if (d == 3) return 79;   // 1001111
  else if (d == 4) return 102;  // 1100110
  else if (d == 5) return 109;  // 1101101
  else if (d == 6) return 125;  // 1111101
  else if (d == 7) return 7;    // 0000111
  else if (d == 8) return 127;  // 1111111
  else if (d == 9) return 111;  // 1101111
  return 0;
}

// Check if segment `seg` is on in `mask` (no bitwise ops in GLSL 100).
bool segOn(int mask, int seg)
{
  float p = pow(2.0, float(seg));
  return mod(floor(float(mask) / p), 2.0) >= 1.0;
}

// Is pixel (x,y) in a 10-wide, 14-tall digit `d` on any active segment?
// Coordinates are top-down (y=0 top, y=13 bottom).
bool digitPixel(int d, float x, float y)
{
  int mask = segmentMask(d);
  // a: top horizontal
  if (y >= 0.0 && y < 2.0 && x >= 1.0 && x < 9.0 && segOn(mask, 0)) return true;
  // b: top-right vertical
  if (y >= 1.0 && y < 7.0 && x >= 8.0 && x < 10.0 && segOn(mask, 1)) return true;
  // c: bottom-right vertical
  if (y >= 7.0 && y < 13.0 && x >= 8.0 && x < 10.0 && segOn(mask, 2)) return true;
  // d: bottom horizontal
  if (y >= 12.0 && y < 14.0 && x >= 1.0 && x < 9.0 && segOn(mask, 3)) return true;
  // e: bottom-left vertical
  if (y >= 7.0 && y < 13.0 && x >= 0.0 && x < 2.0 && segOn(mask, 4)) return true;
  // f: top-left vertical
  if (y >= 1.0 && y < 7.0 && x >= 0.0 && x < 2.0 && segOn(mask, 5)) return true;
  // g: middle horizontal
  if (y >= 6.0 && y < 8.0 && x >= 1.0 && x < 9.0 && segOn(mask, 6)) return true;
  return false;
}

void main()
{
  vec2 frag = gl_FragCoord.xy;
  float vx = frag.x / u_resolution.x;        // 0 left, 1 right
  float vy = 1.0 - frag.y / u_resolution.y;  // 0 top, 1 bottom (video coords)

  vec3 yuv;
  if (vy < 2.0 / 3.0)
    yuv = colorBar8bit(vx);
  else if (vy < 3.0 / 4.0)
    yuv = castellations8bit(vx);
  else
    yuv = plugeArea8bit(vx);

  // Frame counter overlay -- disabled when u_frameCountPos.x < 0
  if (u_frameCountPos.x >= 0.0)
  {
    // Box dimensions: 4 digits at scale 4 with padding
    float scale = 4.0;
    float digitW = 10.0 * scale;       // 40
    float digitH = 14.0 * scale;       // 56
    float space = 2.0 * scale;         // 8
    float pad = space;                 // 8
    float boxW = 4.0 * digitW + 3.0 * space + 2.0 * pad; // 200
    float boxH = digitH + 2.0 * pad;   // 72

    // Convert box top-left from video coords to shader coords, then compute local pos
    // (top-down within box).
    float localX = frag.x - u_frameCountPos.x;
    float localY = (u_resolution.y - u_frameCountPos.y) - frag.y;

    if (localX >= 0.0 && localX < boxW && localY >= 0.0 && localY < boxH)
    {
      // Inside box: start with black background
      yuv = vec3(16.0, 128.0, 128.0);

      // Inside padded region?
      float innerX = localX - pad;
      float innerY = localY - pad;
      if (innerX >= 0.0 && innerY >= 0.0 && innerY < digitH)
      {
        float cellW = digitW + space;
        // Which of the 4 digit cells (left to right = thousands to ones)?
        int digitIdx = int(floor(innerX / cellW));
        float cellX = innerX - float(digitIdx) * cellW;
        if (digitIdx >= 0 && digitIdx < 4 && cellX < digitW)
        {
          // Normalize to 10x14 base coords
          float baseX = cellX / scale;
          float baseY = innerY / scale;

          // Extract the digit at this position from u_frame
          int divisor = 1;
          if (digitIdx == 0) divisor = 1000;
          else if (digitIdx == 1) divisor = 100;
          else if (digitIdx == 2) divisor = 10;
          int d = int(mod(float(u_frame / divisor), 10.0));

          if (digitPixel(d, baseX, baseY))
            yuv = vec3(235.0, 128.0, 128.0); // white digit
        }
      }
    }
  }

  gl_FragColor = vec4(yuv / 255.0, 1.0);
}
