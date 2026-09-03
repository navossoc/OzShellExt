// IThumbnailProvider for .ozb / .ozj / .ozt files.
//
// One class serves all three: the CLSID the shell created decides the format,
// which OzImage turns into a prefix size and a decoder. The decode happens
// inside the thumbnail host process: no helper executable, no temp file, no
// disk round-trip.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "ShellExt.h"
#include "OzImage.h"

#include <shlwapi.h>
#include <thumbcache.h>
#include <new>

#ifdef OZ_ENABLE_THUMBNAIL

// Explorer never asks for more than a few hundred pixels; this only guards
// against a caller passing something absurd.
static const UINT kMaxThumbnailSize = 10000;

class OzThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    explicit OzThumbnailProvider(ozimg::Format format) :
        m_cRef(1), m_stream(nullptr), m_format(format)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(OzThumbnailProvider, IInitializeWithStream),
            QITABENT(OzThumbnailProvider, IThumbnailProvider),
            { nullptr, 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
            delete this;
        return cRef;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD /*grfMode*/) override
    {
        if (!stream)
            return E_INVALIDARG;
        if (m_stream)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_stream = stream;
        m_stream->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override
    {
        if (!phbmp || !pdwAlpha)
            return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;

        if (!m_stream)
            return E_UNEXPECTED;
        if (cx == 0 || cx > kMaxThumbnailSize)
            return E_INVALIDARG;

        ozimg::Image image;
        const HRESULT hr = ozimg::DecodeStream(m_stream, m_format, image);
        if (FAILED(hr))
            return hr;

        // Never enlarge: an image that already fits in cx is returned at its
        // native size.
        const int longest = (image.width > image.height) ? image.width : image.height;
        int dw = image.width;
        int dh = image.height;
        if (static_cast<UINT>(longest) > cx)
        {
            const double scale = static_cast<double>(cx) / longest;
            dw = static_cast<int>(image.width * scale + 0.5);
            dh = static_cast<int>(image.height * scale + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
        }

        // WTSAT_ARGB means straight alpha, which is what the decoders produce.
        HBITMAP bitmap = ozimg::CreateScaledDib(image, dw, dh, false);
        if (!bitmap)
            return E_OUTOFMEMORY;

        *phbmp = bitmap;
        *pdwAlpha = image.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        return S_OK;
    }

private:
    ~OzThumbnailProvider()
    {
        if (m_stream)
            m_stream->Release();
        InterlockedDecrement(&g_cDllRef);
    }

    LONG m_cRef;
    IStream* m_stream;
    ozimg::Format m_format;
};

IUnknown* CreateOzThumbnailProvider(ozimg::Format format)
{
    return static_cast<IInitializeWithStream*>(
        new (std::nothrow) OzThumbnailProvider(format));
}

#endif  // OZ_ENABLE_THUMBNAIL
