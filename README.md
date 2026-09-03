# OzShellExt

In-process Explorer shell extensions for the OZ image formats used by MU
Online: a thumbnail provider and a preview-pane handler for `.ozb`, `.ozj` and
`.ozt`.

*[Leia em português](README.pt-BR.md)*

## The formats

Each one is an ordinary image behind a short prefix:

| extension | prefix | what follows |
| --- | --- | --- |
| `.ozb` | 4 bytes | a BMP |
| `.ozj` | 24 bytes | a JPEG |
| `.ozt` | 4 bytes | a TGA, with alpha |

The prefix is four bytes the decoder ignores - only its length matters.
Across a full client (704 `.ozt` files) 701 start with `00 00 02 00`, which is
just a copy of the first four bytes of the TGA header sitting right behind it,
and three with `00 00 00 00`. What *is* checked is the signature of the image
behind the prefix (`BM`, `FF D8`), which fails fast on a mislabelled file.

BMP and JPEG go through WIC, the imaging component Windows already ships. TGA
has no WIC codec, so `OzImage.cpp` decodes it: image types 1, 2 and 3 with
their RLE twins 9, 10 and 11 - color-mapped, truecolor and grayscale - at 8,
15, 16, 24 and 32 bpp, honouring the origin bits and the color map first-entry
index. Type 0 carries no image data and the TGA 1.0 Huffman variants 32 and 33
are extinct; both are rejected.

Two of the alpha rules come from what real files do rather than from the spec:

- A 32 bpp image keeps its alpha channel even when the descriptor claims zero
  attribute bits. In a stock client 10 files are cut-out foliage, hair and fur
  stored exactly that way; honouring the descriptor turns them into opaque
  rectangles.
- An image whose alpha is zero in every pixel is treated as opaque. A fully
  transparent thumbnail shows nothing, while the color channel often holds a
  complete texture - another 13 files, the grass tiles among them.

## What it does

- **Thumbnails** in File Explorer, at any icon size.
- **Preview pane** rendering, scaled to fit, with the pane's own background and
  a checkerboard behind transparency.

Both run inside the shell's own host process (`dllhost.exe` and
`prevhost.exe`). There is no helper executable to launch, no temp file and no
managed runtime to start.

The two handler kinds are independent: install one, the other, or both.

## Measured

| file | size | target | time |
| --- | --- | --- | --- |
| `Chrome01.OZJ` | 64x64 | 256 px thumbnail | 0.54 ms |
| `blood.OZT` | 128x128 | 256 px thumbnail | 0.42 ms |
| `TerrainHeight.OZB` | 256x256 | 256 px thumbnail | 1.31 ms |

## How it works

One DLL, six CLSIDs - a thumbnail provider and a preview handler per
extension. The CLSID the shell creates is what tells the handler which format
it is looking at, so nothing has to be sniffed out of the file.

| | thumbnail | preview |
| --- | --- | --- |
| `.ozb` | `{247FC569-1E33-4B8A-9DA1-EDD92F062BFD}` | `{7F42AA44-9ED0-45B3-860B-49800BE1D008}` |
| `.ozj` | `{839DC7DB-0F6A-4EE9-8661-4C081C104CB8}` | `{16D991BD-A3E2-4888-BBA7-B37F422F5FE5}` |
| `.ozt` | `{C3DA4352-C864-4609-A239-7F226ECDC41C}` | `{E864CB5D-539C-4F61-BF5C-147EFA19288F}` |

- `OzImage.cpp` - prefix handling, the WIC and TGA decoders, and scaling: box
  filter when minifying, bilinear when magnifying, both weighting color by
  alpha so transparent pixels do not bleed a dark halo into the edges.
- `ThumbnailProvider.cpp` - `IInitializeWithStream` + `IThumbnailProvider`,
  writing straight into a top-down 32 bpp DIB section. Never enlarges; returns
  `WTSAT_ARGB` only when the source actually carries alpha. `ThreadingModel =
  Both`, so the thumbnail host does not have to marshal.
- `PreviewHandler.cpp` - `IPreviewHandler` + `IPreviewHandlerVisuals` +
  `IOleWindow` + `IObjectWithSite` over a plain child window. Scales to fit
  (enlarging when the image is small), honours the pane's background and text
  colors, draws a checkerboard behind transparency, caches the scaled bitmap
  per pane size and double-buffers so dragging the splitter does not flicker.
  `ThreadingModel = Apartment`, `AppID` set to the `prevhost.exe` surrogate.
- `ShellExt.cpp` - the format table, class factory, `DllRegisterServer` /
  `DllUnregisterServer` and `DllInstall`, which is what makes per-handler
  installation possible.

Dependencies: Win32 only (`gdi32`, `msimg32`, `ole32`, `advapi32`, `shlwapi`,
`shell32`, `windowscodecs`). Built with `/MT`, so no VC++ redistributable is
needed.

## Build

```bat
build.cmd
```

Produces `build\OzShellExt.dll` (x64). Needs Visual Studio 2022 Community at
the default path; adjust `VSDEV` in the script otherwise.

`build.cmd thumbnail` and `build.cmd preview` compile a DLL that carries only
that handler kind. You rarely need them: the default build contains both, and
which ones get *registered* is decided at install time.

## Install

Grab the zip from the Releases page, extract it and run `install.cmd` as
administrator - no compiler, no build step. The same scripts work straight
from the source tree after `build.cmd`.

```bat
install.cmd             (as administrator - both handler kinds)
install.cmd thumbnail   (thumbnails only)
install.cmd preview     (preview pane only)

uninstall.cmd           (removes everything)
uninstall.cmd preview   (removes just that handler kind)
```

Each option applies to all three extensions. Installs to
`%ProgramFiles%\navossoc\OzShellExt`; adding the other handler later is another
`install.cmd`, no rebuild.

Under the hood this is `regsvr32 /n /i:<handler>`, which calls `DllInstall`
instead of `DllRegisterServer`. Plain `regsvr32 OzShellExt.dll` still installs
everything the DLL carries.

To see the change, clear the thumbnail cache:

```bat
del /q "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db"
```

## Test

```bat
test\build-test.cmd
cd build
test.exe thumb   some.ozt 256 200
test.exe preview some.ozt 500 400
```

`test.exe` loads the DLL without registering it in COM, picks the CLSID from
the file extension, drives the handler and writes `out.bmp` for inspection. The
preview mode paints through `WM_PRINTCLIENT`, so no window has to be visible.

Point it at real game files - `Data\Effect`, `Data\Interface` and
`Data\World*` are full of all three formats.

## Release

```bat
package.cmd
```

Builds both handler kinds and stages `dist\OzShellExt-<version>-x64.zip` with
the DLL, both install scripts, the license and the READMEs, then prints the
artifact's SHA256 for the release notes. The version comes from `VER_STRING`
in `OzShellExt.rc`, so that is the only place to bump it.

## License

MIT - see [LICENSE](LICENSE).
