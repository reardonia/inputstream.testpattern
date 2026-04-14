# Test Pattern Generator add-on for Kodi

> [!NOTE]
> This add-on requires [Kodi PR #28090](https://github.com/xbmc/xbmc/pull/28090)
> (addon-instance video codec API additions). It will not build against stock Kodi.

<p align="center">
  <img width="800" alt="Color bars from SMPTE BT.709 limited range" src="https://github.com/user-attachments/assets/8082b1ab-10df-436f-9a1f-0ee6b6e48e6c" />
</p>

<p align="center"><i>Color bars from SMPTE BT.709 limited range.</i></p>

This is a [Kodi](https://kodi.tv) input stream add-on that generates synthetic
test patterns from GLSL shaders and feeds them through Kodi's video pipeline
as YUV (or packed RGB) frames. It serves two purposes:

1. **Video pipeline verification** - renderer correctness for limited-range
   compression, 10-bit precision, HDR tone mapping, format conversion,
   deinterlacing, scaling algorithms, and essentially any other stage of
   Kodi's video pipeline.
2. **Display calibration** - standard SMPTE reference patterns for checking
   display response and settings.

Patterns are described by small `.pattern` files (selected via Kodi as if they
were video files). Each pattern references a fragment shader under `media/`
which renders to an offscreen framebuffer; the result is read back and handed
to the Kodi video codec as a `VIDEOCODEC_PICTURE`.

From Kodi's perspective the add-on looks very much like the FFmpeg video
decoder: it supplies decoded frames with full colorspace and HDR metadata
through the standard `VIDEOCODEC_PICTURE` interface. This means test
patterns flow through exactly the same render, deinterlace, scale, and
display paths as real video, so pipeline bugs surface in the same places
they would with real content. And because each pattern is just a small
GLES fragment shader plus a short `.pattern` file, authoring a new test
case is a matter of a few lines of GLSL.

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
