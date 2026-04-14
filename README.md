# Test Pattern Generator add-on for Kodi

This is a [Kodi](https://kodi.tv) input stream add-on that generates synthetic
test patterns from GLSL shaders and feeds them through Kodi's video pipeline
as YUV (or packed RGB) frames. It serves two purposes:

1. **Video pipeline verification** - renderer correctness for limited-range
   compression, 10-bit precision, HDR tone mapping, and format conversion.
2. **Display calibration** - standard SMPTE reference patterns for checking
   display response and settings.

Patterns are described by small `.pattern` files (selected via Kodi as if they
were video files). Each pattern references a fragment shader under `media/`
which renders to an offscreen framebuffer; the result is read back and handed
to the Kodi video codec as a `VIDEOCODEC_PICTURE`.

## Status

Work in progress. The current code depends on additions to Kodi's add-on
video codec API that are not yet merged upstream. It will not build against
stock Kodi until those land.

## Building

Standard Kodi binary add-on build. See the Kodi wiki on
[binary add-ons](https://kodi.wiki/view/Add-on_development#Binary_add-ons)
for the general build procedure.

## Enabling .pattern files

The add-on declares itself as a handler for the `.pattern` extension, but
Kodi will not treat `.pattern` as a playable video file unless it is listed
in its video extensions. A binary add-on cannot modify this list itself, so
you must add it manually to `advancedsettings.xml`:

    <advancedsettings version="1.0">
      <videoextensions>
        <add>.pattern</add>
      </videoextensions>
    </advancedsettings>

`advancedsettings.xml` lives in Kodi's userdata directory (for example
`~/.kodi/userdata/advancedsettings.xml` on Linux). Restart Kodi after
editing. You can then point Kodi at any `.pattern` file under `media/` and
play it like a regular video.

## License

Test Pattern Generator is **[GPLv2+ licensed](LICENSE.md)**.
