// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "OzImage.h"

#include <wincodec.h>
#include <new>

namespace ozimg
{
    size_t PrefixSize(Format format)
    {
        return (format == Format::Ozj) ? 24 : 4;
    }

    HRESULT ReadWholeStream(IStream* stream, std::vector<BYTE>& out)
    {
        STATSTG stat = {};
        ULONGLONG hint = 0;
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)))
        {
            if (stat.cbSize.QuadPart > kMaxStreamBytes)
                return E_OUTOFMEMORY;
            hint = stat.cbSize.QuadPart;
        }

        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        try
        {
            out.clear();
            out.reserve(hint ? static_cast<size_t>(hint) : 64 * 1024);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        BYTE buffer[64 * 1024];
        for (;;)
        {
            ULONG read = 0;
            const HRESULT hr = stream->Read(buffer, sizeof(buffer), &read);
            if (FAILED(hr))
                return hr;
            if (read == 0)
                break;
            if (out.size() + read > kMaxStreamBytes)
                return E_OUTOFMEMORY;
            try
            {
                out.insert(out.end(), buffer, buffer + read);
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            if (hr == S_FALSE)
                break;
        }
        return out.empty() ? E_FAIL : S_OK;
    }

    // ------------------------------------------------------------------- WIC

    template <typename T>
    static void SafeRelease(T*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    // True when the source pixel format can carry transparency. Asking WIC
    // beats guessing from the container: an 8 bpp BMP is opaque, a 32 bpp one
    // may not be.
    static bool PixelFormatHasAlpha(IWICImagingFactory* factory, REFWICPixelFormatGUID guid)
    {
        IWICComponentInfo* info = nullptr;
        if (FAILED(factory->CreateComponentInfo(guid, &info)))
            return false;

        bool hasAlpha = false;
        IWICPixelFormatInfo2* formatInfo = nullptr;
        if (SUCCEEDED(info->QueryInterface(IID_PPV_ARGS(&formatInfo))))
        {
            BOOL supports = FALSE;
            if (SUCCEEDED(formatInfo->SupportsTransparency(&supports)))
                hasAlpha = (supports != FALSE);
            formatInfo->Release();
        }
        info->Release();
        return hasAlpha;
    }

    static HRESULT DecodeWic(const BYTE* data, size_t size, Image& out)
    {
        if (size > MAXDWORD)
            return E_OUTOFMEMORY;

        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr))
            return hr;

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;

        hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
        {
            // InitializeFromMemory does not copy, so `data` has to outlive the
            // decode - it does, the caller owns the buffer for the whole call.
            hr = stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                              static_cast<DWORD>(size));
        }
        if (SUCCEEDED(hr))
        {
            hr = factory->CreateDecoderFromStream(stream, nullptr,
                                                  WICDecodeMetadataCacheOnDemand,
                                                  &decoder);
        }
        if (SUCCEEDED(hr))
            hr = decoder->GetFrame(0, &frame);

        UINT width = 0, height = 0;
        if (SUCCEEDED(hr))
            hr = frame->GetSize(&width, &height);

        if (SUCCEEDED(hr) &&
            (width == 0 || height == 0 ||
             static_cast<ULONGLONG>(width) * height > kMaxPixels))
        {
            hr = E_FAIL;
        }

        if (SUCCEEDED(hr))
        {
            WICPixelFormatGUID sourceFormat = {};
            if (SUCCEEDED(frame->GetPixelFormat(&sourceFormat)))
                out.hasAlpha = PixelFormatHasAlpha(factory, sourceFormat);
        }

        if (SUCCEEDED(hr))
            hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr))
        {
            hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        }

        if (SUCCEEDED(hr))
        {
            const size_t stride = static_cast<size_t>(width) * 4;
            try
            {
                out.rgba.resize(stride * height);
            }
            catch (const std::bad_alloc&)
            {
                hr = E_OUTOFMEMORY;
            }

            if (SUCCEEDED(hr))
            {
                hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                           static_cast<UINT>(out.rgba.size()),
                                           out.rgba.data());
            }
        }

        if (SUCCEEDED(hr))
        {
            out.width = static_cast<int>(width);
            out.height = static_cast<int>(height);
        }

        SafeRelease(converter);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(stream);
        SafeRelease(factory);
        return hr;
    }

    // ------------------------------------------------------------------- TGA

    static unsigned ReadU16(const BYTE* p)
    {
        return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
    }

    // Expands 5-bit channels to 8 bits without a lookup table: 31 -> 255.
    static BYTE Expand5(unsigned v)
    {
        return static_cast<BYTE>((v << 3) | (v >> 2));
    }

    // How to read one sample. Truecolor pixels and color map entries draw from
    // the same set, which is why this is separate from the image type.
    enum class TgaSample
    {
        Gray8,        // 8 bpp grayscale
        GrayAlpha16,  // 16 bpp grayscale plus alpha
        Rgb15,        // 15/16 bpp, A1R5G5B5 little endian
        Bgr24,        // 24 bpp
        Bgra32,       // 32 bpp
    };

    static int SampleBytes(TgaSample sample)
    {
        switch (sample)
        {
        case TgaSample::Gray8: return 1;
        case TgaSample::GrayAlpha16: return 2;
        case TgaSample::Rgb15: return 2;
        case TgaSample::Bgr24: return 3;
        default: return 4;
        }
    }

    // `alphaBit` only matters for Rgb15, where the top bit is the attribute and
    // the header has to say it means something. A 32 bpp sample always keeps
    // its alpha byte: exporters routinely leave the attribute-bit count in the
    // descriptor at zero on files whose alpha is very much in use.
    static void TgaSampleToRgba(const BYTE* src, TgaSample sample, bool alphaBit,
                                BYTE* dst)
    {
        switch (sample)
        {
        case TgaSample::Gray8:
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = 255;
            break;

        case TgaSample::GrayAlpha16:
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = src[1];
            break;

        case TgaSample::Rgb15:
        {
            const unsigned v = ReadU16(src);
            dst[0] = Expand5((v >> 10) & 0x1f);
            dst[1] = Expand5((v >> 5) & 0x1f);
            dst[2] = Expand5(v & 0x1f);
            dst[3] = alphaBit ? ((v & 0x8000) ? 255 : 0) : 255;
            break;
        }

        case TgaSample::Bgr24:
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
            break;

        default:
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            break;
        }
    }

    // Picks how to read a sample of `depth` bits. Grayscale is its own case
    // because 16 bpp means gray plus alpha there, not A1R5G5B5.
    static bool SampleForDepth(int depth, bool grayscale, TgaSample& out)
    {
        if (grayscale)
        {
            if (depth == 8) { out = TgaSample::Gray8; return true; }
            if (depth == 16) { out = TgaSample::GrayAlpha16; return true; }
            return false;
        }

        switch (depth)
        {
        case 8:  out = TgaSample::Gray8;  return true;
        case 15:
        case 16: out = TgaSample::Rgb15;  return true;
        case 24: out = TgaSample::Bgr24;  return true;
        case 32: out = TgaSample::Bgra32; return true;
        default: return false;
        }
    }

    static HRESULT DecodeTga(const BYTE* data, size_t size, Image& out)
    {
        if (size < 18)
            return E_FAIL;

        const BYTE idLength = data[0];
        const BYTE colorMapType = data[1];
        const BYTE imageType = data[2];
        const unsigned cmFirstEntry = ReadU16(data + 3);
        const unsigned cmLength = ReadU16(data + 5);
        const BYTE cmDepth = data[7];
        const int width = static_cast<int>(ReadU16(data + 12));
        const int height = static_cast<int>(ReadU16(data + 14));
        const int depth = data[16];
        const BYTE descriptor = data[17];

        // 9..11 are the run-length encoded twins of 1..3. Type 0 carries no
        // image data, and the TGA 1.0 Huffman variants 32/33 are extinct.
        const bool rle = (imageType >= 9 && imageType <= 11);
        const int baseType = rle ? (imageType - 8) : imageType;

        if (baseType != 1 && baseType != 2 && baseType != 3)
            return E_FAIL;
        if (colorMapType > 1)
            return E_FAIL;
        if (width <= 0 || height <= 0 ||
            static_cast<ULONGLONG>(width) * height > kMaxPixels)
        {
            return E_FAIL;
        }

        // Bits 3..0 of the descriptor hold the attribute-bit count; bit 5 puts
        // the origin at the top and bit 4 at the right.
        const bool attributeBits = (descriptor & 0x0f) != 0;
        const bool flipY = (descriptor & 0x20) == 0;
        const bool flipX = (descriptor & 0x10) != 0;

        // On a color-mapped image `depth` sizes the index, not the color.
        const bool indexed = (baseType == 1);
        TgaSample sample = TgaSample::Bgra32;
        if (!indexed && !SampleForDepth(depth, baseType == 3, sample))
            return E_FAIL;
        if (indexed && depth != 8 && depth != 16)
            return E_FAIL;

        const int bytesPerPixel = (depth + 7) / 8;
        size_t pos = 18u + idLength;

        // The id field can push past the end of a short file. Catch it here, or
        // `size - pos` below wraps around and every bound derived from it is
        // meaningless.
        if (pos > size)
            return E_FAIL;

        const BYTE* palette = nullptr;
        TgaSample paletteSample = TgaSample::Bgr24;
        if (colorMapType == 1)
        {
            if (!SampleForDepth(cmDepth, false, paletteSample))
                return E_FAIL;
            const size_t paletteBytes =
                static_cast<size_t>(cmLength) * SampleBytes(paletteSample);
            if (pos + paletteBytes > size)
                return E_FAIL;
            palette = data + pos;
            pos += paletteBytes;
        }
        // An empty color map passes the bounds check above with zero bytes,
        // and every index would then clamp to entry 0 and read past the end of
        // the buffer - straight into the thumbnail the caller gets back.
        if (indexed && (!palette || cmLength == 0))
            return E_FAIL;

        const size_t pixelCount = static_cast<size_t>(width) * height;
        const size_t rawBytes = pixelCount * bytesPerPixel;

        // Check what the file can actually deliver before allocating for what
        // its header claims. Otherwise a 27 byte file declaring 65535x1525
        // costs half a gigabyte before being rejected, and the shell decodes
        // several files at once.
        if (rle)
        {
            // The cheapest packet that yields pixels is a run: one header byte
            // plus one pixel, for at most 128 pixels. That caps how far the
            // stream can possibly expand, and no valid file exceeds it.
            const size_t avail = size - pos;
            const size_t packets = (avail + bytesPerPixel) / (1u + bytesPerPixel);
            if (pixelCount > packets * 128)
                return E_FAIL;
        }
        else if (pos + rawBytes > size)
        {
            return E_FAIL;
        }

        std::vector<BYTE> raw;
        try
        {
            raw.resize(rawBytes);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        if (rle)
        {
            // Packets: a header byte, then either one pixel to repeat
            // (high bit set) or a run of literal pixels.
            size_t written = 0;
            while (written < rawBytes)
            {
                if (pos >= size)
                    return E_FAIL;
                const BYTE header = data[pos++];
                const int count = (header & 0x7f) + 1;
                const size_t chunk = static_cast<size_t>(count) * bytesPerPixel;

                if (header & 0x80)
                {
                    if (pos + bytesPerPixel > size || written + chunk > rawBytes)
                        return E_FAIL;
                    for (int i = 0; i < count; ++i)
                    {
                        memcpy(&raw[written], data + pos, bytesPerPixel);
                        written += bytesPerPixel;
                    }
                    pos += bytesPerPixel;
                }
                else
                {
                    if (pos + chunk > size || written + chunk > rawBytes)
                        return E_FAIL;
                    memcpy(&raw[written], data + pos, chunk);
                    written += chunk;
                    pos += chunk;
                }
            }
        }
        else
        {
            // Already bounds-checked above, before the allocation.
            memcpy(raw.data(), data + pos, rawBytes);
        }

        try
        {
            out.rgba.resize(pixelCount * 4);
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }

        const int paletteEntry = SampleBytes(paletteSample);

        for (size_t i = 0; i < pixelCount; ++i)
        {
            const int x = static_cast<int>(i % width);
            const int y = static_cast<int>(i / width);
            const int dx = flipX ? (width - 1 - x) : x;
            const int dy = flipY ? (height - 1 - y) : y;
            BYTE* dst = &out.rgba[(static_cast<size_t>(dy) * width + dx) * 4];

            if (indexed)
            {
                // Indices start at cmFirstEntry, so the color map stored in the
                // file holds that entry at offset zero.
                unsigned index = (bytesPerPixel == 2) ? ReadU16(&raw[i * 2])
                                                      : raw[i];
                index = (index >= cmFirstEntry) ? (index - cmFirstEntry) : 0;
                if (index >= cmLength)
                    index = 0;
                TgaSampleToRgba(palette + static_cast<size_t>(index) * paletteEntry,
                                paletteSample, attributeBits, dst);
            }
            else
            {
                TgaSampleToRgba(&raw[i * bytesPerPixel], sample, attributeBits, dst);
            }
        }

        out.width = width;
        out.height = height;
        out.hasAlpha = indexed ? (paletteSample == TgaSample::Bgra32)
                               : (sample == TgaSample::Bgra32 ||
                                  sample == TgaSample::GrayAlpha16 ||
                                  (sample == TgaSample::Rgb15 && attributeBits));

        // An image whose alpha is zero everywhere shows nothing at all, and
        // plenty of real files park a fully populated color channel behind a
        // blank alpha one. Treat those as opaque instead of drawing a hole.
        if (out.hasAlpha)
        {
            bool anyVisible = false;
            for (size_t i = 3; i < out.rgba.size(); i += 4)
            {
                if (out.rgba[i] != 0)
                {
                    anyVisible = true;
                    break;
                }
            }
            if (!anyVisible)
            {
                for (size_t i = 3; i < out.rgba.size(); i += 4)
                    out.rgba[i] = 255;
                out.hasAlpha = false;
            }
        }

        return S_OK;
    }

    // ---------------------------------------------------------------- decode

    HRESULT Decode(const BYTE* data, size_t size, Format format, Image& out)
    {
        const size_t prefix = PrefixSize(format);
        if (!data || size <= prefix)
            return E_FAIL;

        const BYTE* body = data + prefix;
        const size_t bodySize = size - prefix;

        if (format == Format::Ozt)
            return DecodeTga(body, bodySize, out);

        // Cheap guard so a mislabelled file fails here instead of inside WIC.
        if (format == Format::Ozb && (bodySize < 2 || body[0] != 'B' || body[1] != 'M'))
            return E_FAIL;
        if (format == Format::Ozj && (bodySize < 2 || body[0] != 0xFF || body[1] != 0xD8))
            return E_FAIL;

        return DecodeWic(body, bodySize, out);
    }

    HRESULT DecodeStream(IStream* stream, Format format, Image& out)
    {
        if (!stream)
            return E_INVALIDARG;

        std::vector<BYTE> data;
        const HRESULT hr = ReadWholeStream(stream, data);
        if (FAILED(hr))
            return hr;
        return Decode(data.data(), data.size(), format, out);
    }

    // --------------------------------------------------------------- scaling

    // Area average over the source rect that maps to each destination pixel.
    static void ResampleBox(const Image& src, BYTE* dst, int dw, int dh,
                            int dstStride, bool premultiply)
    {
        const int sw = src.width;
        const int sh = src.height;
        const BYTE* base = src.rgba.data();

        for (int y = 0; y < dh; ++y)
        {
            const int y0 = static_cast<int>(static_cast<long long>(y) * sh / dh);
            int y1 = static_cast<int>(static_cast<long long>(y + 1) * sh / dh);
            if (y1 <= y0)
                y1 = y0 + 1;

            BYTE* row = dst + static_cast<size_t>(y) * dstStride;

            for (int x = 0; x < dw; ++x)
            {
                const int x0 = static_cast<int>(static_cast<long long>(x) * sw / dw);
                int x1 = static_cast<int>(static_cast<long long>(x + 1) * sw / dw);
                if (x1 <= x0)
                    x1 = x0 + 1;

                unsigned long long sr = 0, sg = 0, sb = 0, sa = 0;
                for (int sy = y0; sy < y1; ++sy)
                {
                    const BYTE* p = base + (static_cast<size_t>(sy) * sw + x0) * 4;
                    for (int sx = x0; sx < x1; ++sx, p += 4)
                    {
                        const unsigned a = p[3];
                        sr += static_cast<unsigned long long>(p[0]) * a;
                        sg += static_cast<unsigned long long>(p[1]) * a;
                        sb += static_cast<unsigned long long>(p[2]) * a;
                        sa += a;
                    }
                }

                const unsigned long long count =
                    static_cast<unsigned long long>(x1 - x0) * (y1 - y0);

                BYTE* o = row + static_cast<size_t>(x) * 4;
                if (sa == 0)
                {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }

                const unsigned alpha = static_cast<unsigned>(sa / count);
                if (premultiply)
                {
                    // sr/count is already color*alpha averaged, i.e. exactly
                    // the premultiplied value.
                    o[0] = static_cast<BYTE>(sb / count / 255);
                    o[1] = static_cast<BYTE>(sg / count / 255);
                    o[2] = static_cast<BYTE>(sr / count / 255);
                }
                else
                {
                    o[0] = static_cast<BYTE>(sb / sa);
                    o[1] = static_cast<BYTE>(sg / sa);
                    o[2] = static_cast<BYTE>(sr / sa);
                }
                o[3] = static_cast<BYTE>(alpha);
            }
        }
    }

    static void ResampleBilinear(const Image& src, BYTE* dst, int dw, int dh,
                                 int dstStride, bool premultiply)
    {
        const int sw = src.width;
        const int sh = src.height;
        const BYTE* base = src.rgba.data();

        // Map destination pixel centers back to source pixel centers.
        const float xRatio = static_cast<float>(sw) / dw;
        const float yRatio = static_cast<float>(sh) / dh;

        for (int y = 0; y < dh; ++y)
        {
            float fy = (y + 0.5f) * yRatio - 0.5f;
            if (fy < 0) fy = 0;
            int y0 = static_cast<int>(fy);
            if (y0 > sh - 1) y0 = sh - 1;
            int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
            const float wy = fy - y0;

            BYTE* row = dst + static_cast<size_t>(y) * dstStride;

            for (int x = 0; x < dw; ++x)
            {
                float fx = (x + 0.5f) * xRatio - 0.5f;
                if (fx < 0) fx = 0;
                int x0 = static_cast<int>(fx);
                if (x0 > sw - 1) x0 = sw - 1;
                int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
                const float wx = fx - x0;

                const BYTE* p00 = base + (static_cast<size_t>(y0) * sw + x0) * 4;
                const BYTE* p01 = base + (static_cast<size_t>(y0) * sw + x1) * 4;
                const BYTE* p10 = base + (static_cast<size_t>(y1) * sw + x0) * 4;
                const BYTE* p11 = base + (static_cast<size_t>(y1) * sw + x1) * 4;

                const float w00 = (1 - wx) * (1 - wy);
                const float w01 = wx * (1 - wy);
                const float w10 = (1 - wx) * wy;
                const float w11 = wx * wy;

                // Weight color by alpha, same reason as the box filter.
                const float a00 = p00[3] * w00, a01 = p01[3] * w01;
                const float a10 = p10[3] * w10, a11 = p11[3] * w11;
                const float sa = a00 + a01 + a10 + a11;

                BYTE* o = row + static_cast<size_t>(x) * 4;
                if (sa <= 0.0f)
                {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }

                const float sr = p00[0] * a00 + p01[0] * a01 + p10[0] * a10 + p11[0] * a11;
                const float sg = p00[1] * a00 + p01[1] * a01 + p10[1] * a10 + p11[1] * a11;
                const float sb = p00[2] * a00 + p01[2] * a01 + p10[2] * a10 + p11[2] * a11;

                const float alpha = sa;  // weights sum to 1, so this is the average
                const float scale = premultiply ? (alpha / 255.0f) : 1.0f;

                o[0] = static_cast<BYTE>(sb / sa * scale + 0.5f);
                o[1] = static_cast<BYTE>(sg / sa * scale + 0.5f);
                o[2] = static_cast<BYTE>(sr / sa * scale + 0.5f);
                o[3] = static_cast<BYTE>(alpha + 0.5f);
            }
        }
    }

    void Resample(const Image& src, BYTE* dst, int dw, int dh, int dstStride,
                  bool premultiply)
    {
        if (!src.valid() || dw <= 0 || dh <= 0)
            return;

        if (dw < src.width || dh < src.height)
            ResampleBox(src, dst, dw, dh, dstStride, premultiply);
        else
            ResampleBilinear(src, dst, dw, dh, dstStride, premultiply);
    }

    HBITMAP CreateScaledDib(const Image& src, int dw, int dh, bool premultiply)
    {
        if (!src.valid() || dw <= 0 || dh <= 0)
            return nullptr;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = dw;
        bmi.bmiHeader.biHeight = -dh;  // negative: top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                          nullptr, 0);
        if (!bitmap || !bits)
        {
            if (bitmap)
                DeleteObject(bitmap);
            return nullptr;
        }

        Resample(src, static_cast<BYTE*>(bits), dw, dh, dw * 4, premultiply);
        return bitmap;
    }
}
