// SMPTE 100% color bars in BT.709 limited range YCbCr, with optional frame counter.
// Output: gl_FragColor.rgb = vec3(Y, Cb, Cr) normalized to [0,1]
// Values computed from BT.709 matrix coefficients (Kr=0.2126, Kb=0.0722)
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

// BT.709 limited-range 8-bit YCbCr values for 100% color bars.
vec3 colorBar8bit(float x)
{
  if (x < 1.0 / 7.0)      return vec3(235.0, 128.0, 128.0); // white
  else if (x < 2.0 / 7.0) return vec3(219.0,  16.0, 138.0); // yellow
  else if (x < 3.0 / 7.0) return vec3(188.0, 154.0,  16.0); // cyan
  else if (x < 4.0 / 7.0) return vec3(173.0,  42.0,  26.0); // green
  else if (x < 5.0 / 7.0) return vec3( 78.0, 214.0, 230.0); // magenta
  else if (x < 6.0 / 7.0) return vec3( 63.0, 102.0, 240.0); // red
  else                    return vec3( 32.0, 240.0, 118.0); // blue
}

// Reverse castellations strip (for chrominance alignment).
vec3 castellations8bit(float x)
{
  if (x < 1.0 / 7.0)      return vec3( 32.0, 240.0, 118.0); // blue
  else if (x < 2.0 / 7.0) return vec3( 16.0, 128.0, 128.0); // black
  else if (x < 3.0 / 7.0) return vec3( 78.0, 214.0, 230.0); // magenta
  else if (x < 4.0 / 7.0) return vec3( 16.0, 128.0, 128.0); // black
  else if (x < 5.0 / 7.0) return vec3(188.0, 154.0,  16.0); // cyan
  else if (x < 6.0 / 7.0) return vec3( 16.0, 128.0, 128.0); // black
  else                    return vec3(235.0, 128.0, 128.0); // white
}

// Bottom pluge area: -I, white, +Q, black, pluge(-4/0/+4), black
// Bottom 1/4 area, SMPTE ECR 1-1978 layout.
// Six boxes across the width, split 4+2:
//   Boxes 1-4 cover the left 5/7 (each 5/28, aligned with the first 5 bars).
//   Boxes 5-6 cover the right 2/7 (each 4/28 = 1/7).
//   1: -I    2: 100% white    3: +Q    4: black    5: PLUGE    6: black
vec3 plugeArea8bit(float x)
{
  float cell = x * 28.0;
  if (cell < 5.0)       return vec3( 16.0, 198.0,  78.0); // -I (approx)
  else if (cell < 10.0) return vec3(235.0, 128.0, 128.0); // 100% white
  else if (cell < 15.0) return vec3( 38.0, 162.0, 195.0); // +Q (approx)
  else if (cell < 20.0) return vec3( 16.0, 128.0, 128.0); // black
  else if (cell < 24.0)
  {
    // 3 narrow PLUGE stripes within box 5
    float pcell = (cell - 20.0) * 3.0 / 4.0;
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
  float vy = 1.0 - frag.y / u_resolution.y;

  vec3 yuv;
  if (vy < 2.0 / 3.0)
    yuv = colorBar8bit(vx);
  else if (vy < 3.0 / 4.0)
    yuv = castellations8bit(vx);
  else
    yuv = plugeArea8bit(vx);

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

    float localX = frag.x - u_frameCountPos.x;
    float localY = (u_resolution.y - u_frameCountPos.y) - frag.y;

    if (localX >= 0.0 && localX < boxW && localY >= 0.0 && localY < boxH)
    {
      yuv = vec3(16.0, 128.0, 128.0);

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
            yuv = vec3(235.0, 128.0, 128.0);
        }
      }
    }
  }

  gl_FragColor = vec4(yuv / 255.0, 1.0);
}
