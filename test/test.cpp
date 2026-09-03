// Test harness: loads the DLL without registering it in COM, drives one of the
// two handlers and writes the result to out.bmp for inspection. The CLSID comes
// from the file extension, the same way the shell picks one.
//
//   test.exe thumb   <file.ozb|.ozj|.ozt> [cx] [iterations]
//   test.exe preview <file.ozb|.ozj|.ozt> [width] [height]
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <objbase.h>
#include <stdio.h>

typedef HRESULT(STDAPICALLTYPE* PFN_DllGetClassObject)(REFCLSID, REFIID, void**);

static const CLSID CLSID_OzbThumbnail =
{ 0x247fc569, 0x1e33, 0x4b8a, { 0x9d, 0xa1, 0xed, 0xd9, 0x2f, 0x06, 0x2b, 0xfd } };
static const CLSID CLSID_OzbPreview =
{ 0x7f42aa44, 0x9ed0, 0x45b3, { 0x86, 0x0b, 0x49, 0x80, 0x0b, 0xe1, 0xd0, 0x08 } };
static const CLSID CLSID_OzjThumbnail =
{ 0x839dc7db, 0x0f6a, 0x4ee9, { 0x86, 0x61, 0x4c, 0x08, 0x1c, 0x10, 0x4c, 0xb8 } };
static const CLSID CLSID_OzjPreview =
{ 0x16d991bd, 0xa3e2, 0x4888, { 0xbb, 0xa7, 0xb3, 0x7f, 0x42, 0x2f, 0x5f, 0xe5 } };
static const CLSID CLSID_OztThumbnail =
{ 0xc3da4352, 0xc864, 0x4609, { 0xa2, 0x39, 0x7f, 0x22, 0x6e, 0xcd, 0xc4, 0x1c } };
static const CLSID CLSID_OztPreview =
{ 0xe864cb5d, 0x539c, 0x4f61, { 0xbf, 0x5c, 0x14, 0x7e, 0xfa, 0x19, 0x28, 0x8f } };

// Picks the CLSID the shell would have used for this extension.
static const CLSID* ClsidFor(const wchar_t* path, bool preview)
{
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext)
        return nullptr;
    if (_wcsicmp(ext, L".ozb") == 0)
        return preview ? &CLSID_OzbPreview : &CLSID_OzbThumbnail;
    if (_wcsicmp(ext, L".ozj") == 0)
        return preview ? &CLSID_OzjPreview : &CLSID_OzjThumbnail;
    if (_wcsicmp(ext, L".ozt") == 0)
        return preview ? &CLSID_OztPreview : &CLSID_OztThumbnail;
    return nullptr;
}

static PFN_DllGetClassObject g_getClassObject = nullptr;

static bool LoadExtension()
{
    HMODULE dll = LoadLibraryW(L"OzShellExt.dll");
    if (!dll)
    {
        wprintf(L"LoadLibrary failed: %lu\n", GetLastError());
        return false;
    }
    g_getClassObject = reinterpret_cast<PFN_DllGetClassObject>(
        GetProcAddress(dll, "DllGetClassObject"));
    if (!g_getClassObject)
    {
        wprintf(L"DllGetClassObject is not exported\n");
        return false;
    }
    return true;
}

static HRESULT CreateHandler(REFCLSID clsid, REFIID riid, void** ppv)
{
    IClassFactory* factory = nullptr;
    HRESULT hr = g_getClassObject(clsid, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return hr;
    hr = factory->CreateInstance(nullptr, riid, ppv);
    factory->Release();
    return hr;
}

static bool SaveBitmap(HBITMAP bitmap, const wchar_t* path)
{
    BITMAP bm = {};
    if (!GetObject(bitmap, sizeof(bm), &bm))
        return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const DWORD size = bm.bmWidth * bm.bmHeight * 4;
    BYTE* bits = new BYTE[size];

    HDC dc = GetDC(nullptr);
    const int scanned = GetDIBits(dc, bitmap, 0, bm.bmHeight, bits, &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (scanned == 0)
    {
        delete[] bits;
        return false;
    }

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + size;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        delete[] bits;
        return false;
    }
    DWORD written = 0;
    WriteFile(file, &fh, sizeof(fh), &written, nullptr);
    WriteFile(file, &bmi.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
    WriteFile(file, bits, size, &written, nullptr);
    CloseHandle(file);
    delete[] bits;
    return true;
}

// ------------------------------------------------------------------ thumbnail

static int RunThumbnail(const wchar_t* input, UINT cx, int iterations)
{
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    HBITMAP bitmap = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;

    for (int i = 0; i < iterations; ++i)
    {
        if (bitmap)
        {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }

        IStream* stream = nullptr;
        HRESULT hr = SHCreateStreamOnFileEx(input, STGM_READ | STGM_SHARE_DENY_WRITE,
                                            0, FALSE, nullptr, &stream);
        if (FAILED(hr))
        {
            wprintf(L"cannot open %s: 0x%08X\n", input, hr);
            return 1;
        }

        IInitializeWithStream* init = nullptr;
        hr = CreateHandler(*ClsidFor(input, false), IID_PPV_ARGS(&init));
        if (FAILED(hr))
        {
            wprintf(L"CreateInstance: 0x%08X\n", hr);
            return 1;
        }

        hr = init->Initialize(stream, 0);
        if (FAILED(hr))
        {
            wprintf(L"Initialize: 0x%08X\n", hr);
            return 1;
        }

        IThumbnailProvider* provider = nullptr;
        init->QueryInterface(IID_PPV_ARGS(&provider));
        hr = provider->GetThumbnail(cx, &bitmap, &alpha);

        provider->Release();
        init->Release();
        stream->Release();

        if (FAILED(hr))
        {
            wprintf(L"GetThumbnail: 0x%08X\n", hr);
            return 1;
        }
    }

    QueryPerformanceCounter(&t1);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

    BITMAP bm = {};
    GetObject(bitmap, sizeof(bm), &bm);
    wprintf(L"ok: %ldx%ld, %d bpp, alpha=%d\n", bm.bmWidth, bm.bmHeight,
            bm.bmBitsPixel, alpha);
    wprintf(L"%d iterations in %.2f ms (%.3f ms per thumbnail)\n",
            iterations, ms, ms / iterations);

    if (SaveBitmap(bitmap, L"out.bmp"))
        wprintf(L"wrote out.bmp\n");

    DeleteObject(bitmap);
    return 0;
}

// -------------------------------------------------------------------- preview

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunPreview(const wchar_t* input, int width, int height)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OzPreviewTestHost";
    RegisterClassExW(&wc);

    HWND host = CreateWindowExW(0, wc.lpszClassName, L"host", WS_OVERLAPPEDWINDOW,
                                0, 0, width, height, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!host)
    {
        wprintf(L"cannot create the host window: %lu\n", GetLastError());
        return 1;
    }

    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(input, STGM_READ | STGM_SHARE_DENY_WRITE,
                                        0, FALSE, nullptr, &stream);
    if (FAILED(hr))
    {
        wprintf(L"cannot open %s: 0x%08X\n", input, hr);
        return 1;
    }

    IInitializeWithStream* init = nullptr;
    hr = CreateHandler(*ClsidFor(input, true), IID_PPV_ARGS(&init));
    if (FAILED(hr))
    {
        wprintf(L"CreateInstance: 0x%08X\n", hr);
        return 1;
    }

    hr = init->Initialize(stream, 0);
    if (FAILED(hr))
    {
        wprintf(L"Initialize: 0x%08X\n", hr);
        return 1;
    }

    IPreviewHandler* preview = nullptr;
    hr = init->QueryInterface(IID_PPV_ARGS(&preview));
    if (FAILED(hr))
    {
        wprintf(L"QueryInterface(IPreviewHandler): 0x%08X\n", hr);
        return 1;
    }

    // Same call order Explorer uses.
    RECT rc = { 0, 0, width, height };
    preview->SetWindow(host, &rc);

    IPreviewHandlerVisuals* visuals = nullptr;
    if (SUCCEEDED(preview->QueryInterface(IID_PPV_ARGS(&visuals))))
    {
        visuals->SetBackgroundColor(GetSysColor(COLOR_WINDOW));
        visuals->SetTextColor(GetSysColor(COLOR_WINDOWTEXT));
        visuals->Release();
    }

    preview->SetRect(&rc);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = preview->DoPreview();
    QueryPerformanceCounter(&t1);

    if (FAILED(hr))
    {
        wprintf(L"DoPreview: 0x%08X\n", hr);
        return 1;
    }
    wprintf(L"DoPreview: %.2f ms\n",
            (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);

    IOleWindow* oleWindow = nullptr;
    HWND pane = nullptr;
    if (SUCCEEDED(preview->QueryInterface(IID_PPV_ARGS(&oleWindow))))
    {
        oleWindow->GetWindow(&pane);
        oleWindow->Release();
    }
    if (!pane)
    {
        wprintf(L"the handler did not create a window\n");
        return 1;
    }

    // Paint into a bitmap through WM_PRINTCLIENT: no visible window needed and
    // the result is deterministic.
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(mem, bmp);
    SendMessageW(pane, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT);
    SelectObject(mem, old);

    if (SaveBitmap(bmp, L"out.bmp"))
        wprintf(L"ok: %dx%d pane, wrote out.bmp\n", width, height);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    preview->Unload();
    preview->Release();
    init->Release();
    stream->Release();
    DestroyWindow(host);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"usage: test.exe thumb   <file.ozb|.ozj|.ozt> [cx] [iterations]\n");
        wprintf(L"       test.exe preview <file.ozb|.ozj|.ozt> [width] [height]\n");
        return 1;
    }

    const wchar_t* mode = argv[1];
    const wchar_t* input = argv[2];

    if (!ClsidFor(input, false))
    {
        wprintf(L"unsupported extension: %s\n", input);
        return 1;
    }

    // Apartment threaded: the preview handler owns a window.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!LoadExtension())
        return 1;

    int rc = 1;
    if (_wcsicmp(mode, L"thumb") == 0)
    {
        rc = RunThumbnail(input,
                          (argc > 3) ? _wtoi(argv[3]) : 256,
                          (argc > 4) ? _wtoi(argv[4]) : 1);
    }
    else if (_wcsicmp(mode, L"preview") == 0)
    {
        rc = RunPreview(input,
                        (argc > 3) ? _wtoi(argv[3]) : 400,
                        (argc > 4) ? _wtoi(argv[4]) : 400);
    }
    else
    {
        wprintf(L"unknown mode: %s\n", mode);
    }

    CoUninitialize();
    return rc;
}
