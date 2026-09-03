// Decoding and scaling shared by both shell extensions.
//
// The OZ formats are ordinary images behind a short prefix:
//
//   .ozb  4 bytes  then a BMP
//   .ozj  24 bytes then a JPEG
//   .ozt  4 bytes  then a TGA (with alpha)
//
// BMP and JPEG go through WIC, the imaging component Windows already ships.
// TGA has no WIC codec, so it is decoded here.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <vector>

namespace ozimg
{
    enum class Format
    {
        Ozb,  // BMP
        Ozj,  // JPEG
        Ozt,  // TGA
    };

    // Bytes to skip before the embedded image starts.
    size_t PrefixSize(Format format);

    // Sanity limits. Anything past these is not worth previewing.
    const ULONGLONG kMaxStreamBytes = 512ull * 1024 * 1024;
    const ULONGLONG kMaxPixels = 100ull * 1000 * 1000;

    // A decoded image, always 4 channels, RGBA, straight (un-premultiplied)
    // alpha.
    struct Image
    {
        std::vector<BYTE> rgba;
        int width = 0;
        int height = 0;
        bool hasAlpha = false;

        bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
    };

    // Reads the whole stream into `out`. Uses Stat() to size the buffer up
    // front and falls back to incremental growth when the size is unknown.
    HRESULT ReadWholeStream(IStream* stream, std::vector<BYTE>& out);

    // Strips the prefix and decodes what is behind it.
    HRESULT Decode(const BYTE* data, size_t size, Format format, Image& out);

    HRESULT DecodeStream(IStream* stream, Format format, Image& out);

    // Scales `src` into a 32 bpp BGRA buffer.
    //
    // Minification uses a box filter, magnification bilinear; both weight the
    // color by alpha. Without that weighting the (arbitrary) color of fully
    // transparent pixels bleeds into the edges and shows up as a dark halo.
    //
    // `premultiply` controls the output: premultiplied for AlphaBlend, straight
    // for a thumbnail handed back as WTSAT_ARGB.
    void Resample(const Image& src, BYTE* dst, int dw, int dh, int dstStride,
                  bool premultiply);

    // Creates a top-down 32 bpp DIB section holding `src` scaled to dw x dh.
    // Returns NULL on failure.
    HBITMAP CreateScaledDib(const Image& src, int dw, int dh, bool premultiply);
}
