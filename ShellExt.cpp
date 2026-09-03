// DLL entry points: class factory, COM registration and unregistration.
//
// Three extensions times two handler kinds means six CLSIDs, all driven from
// the kFormats table below. DllRegisterServer installs everything compiled into
// the DLL; DllInstall installs one kind, which is how regsvr32 lets each user
// pick:
//
//   regsvr32 /n /i:thumbnail OzShellExt.dll
//   regsvr32 /n /i:preview   OzShellExt.dll
//   regsvr32 /u /n /i:preview OzShellExt.dll
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#include "ShellExt.h"

#include <shlwapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <new>

HINSTANCE g_hInst = nullptr;
LONG g_cDllRef = 0;

// {247FC569-1E33-4B8A-9DA1-EDD92F062BFD}
static const CLSID CLSID_OzbThumbnail =
{ 0x247fc569, 0x1e33, 0x4b8a, { 0x9d, 0xa1, 0xed, 0xd9, 0x2f, 0x06, 0x2b, 0xfd } };
// {7F42AA44-9ED0-45B3-860B-49800BE1D008}
static const CLSID CLSID_OzbPreview =
{ 0x7f42aa44, 0x9ed0, 0x45b3, { 0x86, 0x0b, 0x49, 0x80, 0x0b, 0xe1, 0xd0, 0x08 } };
// {839DC7DB-0F6A-4EE9-8661-4C081C104CB8}
static const CLSID CLSID_OzjThumbnail =
{ 0x839dc7db, 0x0f6a, 0x4ee9, { 0x86, 0x61, 0x4c, 0x08, 0x1c, 0x10, 0x4c, 0xb8 } };
// {16D991BD-A3E2-4888-BBA7-B37F422F5FE5}
static const CLSID CLSID_OzjPreview =
{ 0x16d991bd, 0xa3e2, 0x4888, { 0xbb, 0xa7, 0xb3, 0x7f, 0x42, 0x2f, 0x5f, 0xe5 } };
// {C3DA4352-C864-4609-A239-7F226ECDC41C}
static const CLSID CLSID_OztThumbnail =
{ 0xc3da4352, 0xc864, 0x4609, { 0xa2, 0x39, 0x7f, 0x22, 0x6e, 0xcd, 0xc4, 0x1c } };
// {E864CB5D-539C-4F61-BF5C-147EFA19288F}
static const CLSID CLSID_OztPreview =
{ 0xe864cb5d, 0x539c, 0x4f61, { 0xbf, 0x5c, 0x14, 0x7e, 0xfa, 0x19, 0x28, 0x8f } };

struct FormatEntry
{
    ozimg::Format format;
    const wchar_t* ext;
    const CLSID* thumbnailClsid;
    const wchar_t* thumbnailClsidText;
    const wchar_t* thumbnailName;
    const CLSID* previewClsid;
    const wchar_t* previewClsidText;
    const wchar_t* previewName;
};

static const FormatEntry kFormats[] = {
    { ozimg::Format::Ozb, L".ozb",
      &CLSID_OzbThumbnail, L"{247FC569-1E33-4B8A-9DA1-EDD92F062BFD}", L"OZB Thumbnail Provider",
      &CLSID_OzbPreview,   L"{7F42AA44-9ED0-45B3-860B-49800BE1D008}", L"OZB Preview Handler" },
    { ozimg::Format::Ozj, L".ozj",
      &CLSID_OzjThumbnail, L"{839DC7DB-0F6A-4EE9-8661-4C081C104CB8}", L"OZJ Thumbnail Provider",
      &CLSID_OzjPreview,   L"{16D991BD-A3E2-4888-BBA7-B37F422F5FE5}", L"OZJ Preview Handler" },
    { ozimg::Format::Ozt, L".ozt",
      &CLSID_OztThumbnail, L"{C3DA4352-C864-4609-A239-7F226ECDC41C}", L"OZT Thumbnail Provider",
      &CLSID_OztPreview,   L"{E864CB5D-539C-4F61-BF5C-147EFA19288F}", L"OZT Preview Handler" },
};

// Shell interface IDs the extensions register under, per file extension.
static const wchar_t* const kThumbnailInterface =
    L"{E357FCCD-A995-4576-B01F-234630154E96}";
static const wchar_t* const kPreviewInterface =
    L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

// prevhost.exe, the surrogate every preview handler runs inside.
static const wchar_t* const kPreviewHostAppId =
    L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";

static const wchar_t* const kPreviewHandlerListKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
static const wchar_t* const kApprovedKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

// ------------------------------------------------------------- class factory

class ClassFactory : public IClassFactory
{
public:
    typedef IUnknown* (*Creator)(ozimg::Format);

    ClassFactory(Creator creator, ozimg::Format format) :
        m_cRef(1), m_creator(creator), m_format(format)
    {
        InterlockedIncrement(&g_cDllRef);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(ClassFactory, IClassFactory),
            { nullptr, 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
            delete this;
        return cRef;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;

        IUnknown* instance = m_creator(m_format);
        if (!instance)
            return E_OUTOFMEMORY;

        const HRESULT hr = instance->QueryInterface(riid, ppv);
        instance->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock)
            InterlockedIncrement(&g_cDllRef);
        else
            InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }

private:
    ~ClassFactory() { InterlockedDecrement(&g_cDllRef); }

    LONG m_cRef;
    Creator m_creator;
    ozimg::Format m_format;
};

// ------------------------------------------------------------ registry helpers

static LONG SetValue(HKEY root, const wchar_t* subKey, const wchar_t* name,
                     const wchar_t* value)
{
    return RegSetKeyValueW(root, subKey, name, REG_SZ, value,
                           static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void DeleteClsid(const wchar_t* clsidText)
{
    wchar_t key[128];
    StringCchPrintfW(key, ARRAYSIZE(key), L"CLSID\\%s", clsidText);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, key);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
}

// Only clears the association if it still points at us; otherwise another
// handler has already taken the extension over.
static void DeleteShellexIfOurs(const wchar_t* shellexKey, const wchar_t* clsidText)
{
    wchar_t current[128] = {};
    DWORD cb = sizeof(current);
    if (RegGetValueW(HKEY_CLASSES_ROOT, shellexKey, nullptr, RRF_RT_REG_SZ,
                     nullptr, current, &cb) == ERROR_SUCCESS &&
        _wcsicmp(current, clsidText) == 0)
    {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, shellexKey);
        RegDeleteKeyW(HKEY_CLASSES_ROOT, shellexKey);
    }
}

static void DeleteListValue(const wchar_t* listKey, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, listKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
}

static HRESULT GetModulePath(wchar_t (&path)[MAX_PATH])
{
    if (GetModuleFileNameW(g_hInst, path, MAX_PATH) == 0)
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

// ------------------------------------------------------- per-handler registration

// The COM server half, identical for both handler kinds apart from the
// threading model.
static HRESULT RegisterServer(const wchar_t* clsidText, const wchar_t* name,
                              const wchar_t* threadingModel, const wchar_t* appId)
{
    wchar_t modulePath[MAX_PATH] = {};
    HRESULT hr = GetModulePath(modulePath);
    if (FAILED(hr))
        return hr;

    wchar_t clsidKey[128];
    wchar_t inprocKey[160];
    StringCchPrintfW(clsidKey, ARRAYSIZE(clsidKey), L"CLSID\\%s", clsidText);
    StringCchPrintfW(inprocKey, ARRAYSIZE(inprocKey), L"CLSID\\%s\\InprocServer32",
                     clsidText);

    LONG rc = SetValue(HKEY_CLASSES_ROOT, clsidKey, nullptr, name);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    SetValue(HKEY_CLASSES_ROOT, clsidKey, L"DisplayName", name);

    if (appId)
    {
        rc = SetValue(HKEY_CLASSES_ROOT, clsidKey, L"AppID", appId);
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);
    }

    rc = SetValue(HKEY_CLASSES_ROOT, inprocKey, nullptr, modulePath);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);
    rc = SetValue(HKEY_CLASSES_ROOT, inprocKey, L"ThreadingModel", threadingModel);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    SetValue(HKEY_LOCAL_MACHINE, kApprovedKey, clsidText, name);
    return S_OK;
}

static void AssociationKey(const wchar_t* ext, const wchar_t* interfaceId,
                           wchar_t (&out)[160])
{
    StringCchPrintfW(out, ARRAYSIZE(out), L"%s\\shellex\\%s", ext, interfaceId);
}

#ifdef OZ_ENABLE_THUMBNAIL
static HRESULT RegisterThumbnail(const FormatEntry& entry)
{
    // "Both": the thumbnail host calls from MTA threads, and "Apartment" would
    // force needless marshalling.
    HRESULT hr = RegisterServer(entry.thumbnailClsidText, entry.thumbnailName,
                                L"Both", nullptr);
    if (FAILED(hr))
        return hr;

    wchar_t key[160];
    AssociationKey(entry.ext, kThumbnailInterface, key);
    const LONG rc = SetValue(HKEY_CLASSES_ROOT, key, nullptr, entry.thumbnailClsidText);
    return (rc == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(rc);
}

static void UnregisterThumbnail(const FormatEntry& entry)
{
    wchar_t key[160];
    AssociationKey(entry.ext, kThumbnailInterface, key);
    DeleteShellexIfOurs(key, entry.thumbnailClsidText);
    DeleteClsid(entry.thumbnailClsidText);
    DeleteListValue(kApprovedKey, entry.thumbnailClsidText);
}
#endif  // OZ_ENABLE_THUMBNAIL

#ifdef OZ_ENABLE_PREVIEW
static HRESULT RegisterPreview(const FormatEntry& entry)
{
    // A preview handler owns a window, so it is bound to an apartment, and
    // without the AppID it would load inside Explorer itself.
    HRESULT hr = RegisterServer(entry.previewClsidText, entry.previewName,
                                L"Apartment", kPreviewHostAppId);
    if (FAILED(hr))
        return hr;

    wchar_t key[160];
    AssociationKey(entry.ext, kPreviewInterface, key);
    LONG rc = SetValue(HKEY_CLASSES_ROOT, key, nullptr, entry.previewClsidText);
    if (rc != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(rc);

    // The shell only honours preview handlers listed here.
    rc = SetValue(HKEY_LOCAL_MACHINE, kPreviewHandlerListKey,
                  entry.previewClsidText, entry.previewName);
    return (rc == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(rc);
}

static void UnregisterPreview(const FormatEntry& entry)
{
    wchar_t key[160];
    AssociationKey(entry.ext, kPreviewInterface, key);
    DeleteShellexIfOurs(key, entry.previewClsidText);
    DeleteClsid(entry.previewClsidText);
    DeleteListValue(kPreviewHandlerListKey, entry.previewClsidText);
    DeleteListValue(kApprovedKey, entry.previewClsidText);
}
#endif  // OZ_ENABLE_PREVIEW

// Harmless to write more than once, and it belongs to the file type rather
// than to either handler.
static void RegisterFileTypes()
{
    for (const FormatEntry& entry : kFormats)
        SetValue(HKEY_CLASSES_ROOT, entry.ext, L"PerceivedType", L"image");
}

static HRESULT RegisterAll(bool thumbnail, bool preview)
{
    for (const FormatEntry& entry : kFormats)
    {
#ifdef OZ_ENABLE_THUMBNAIL
        if (thumbnail)
        {
            const HRESULT hr = RegisterThumbnail(entry);
            if (FAILED(hr))
                return hr;
        }
#else
        UNREFERENCED_PARAMETER(thumbnail);
#endif
#ifdef OZ_ENABLE_PREVIEW
        if (preview)
        {
            const HRESULT hr = RegisterPreview(entry);
            if (FAILED(hr))
                return hr;
        }
#else
        UNREFERENCED_PARAMETER(preview);
#endif
    }

    RegisterFileTypes();
    return S_OK;
}

static void UnregisterAll(bool thumbnail, bool preview)
{
    for (const FormatEntry& entry : kFormats)
    {
#ifdef OZ_ENABLE_THUMBNAIL
        if (thumbnail)
            UnregisterThumbnail(entry);
#else
        UNREFERENCED_PARAMETER(thumbnail);
#endif
#ifdef OZ_ENABLE_PREVIEW
        if (preview)
            UnregisterPreview(entry);
#else
        UNREFERENCED_PARAMETER(preview);
#endif
    }
}

// ---------------------------------------------------------------- entry points

STDAPI DllRegisterServer()
{
    const HRESULT hr = RegisterAll(true, true);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}

STDAPI DllUnregisterServer()
{
    UnregisterAll(true, true);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

// regsvr32 /n /i:<what>, with <what> being "thumbnail", "preview" or "all".
// An empty command line means "all", matching DllRegisterServer.
STDAPI DllInstall(BOOL bInstall, PCWSTR pszCmdLine)
{
    const wchar_t* what = (pszCmdLine && *pszCmdLine) ? pszCmdLine : L"all";

    const bool all = (_wcsicmp(what, L"all") == 0);
    const bool thumbnail = all || (_wcsicmp(what, L"thumbnail") == 0);
    const bool preview = all || (_wcsicmp(what, L"preview") == 0);

    if (!thumbnail && !preview)
        return E_INVALIDARG;  // unknown keyword

#ifndef OZ_ENABLE_THUMBNAIL
    if (thumbnail && !all)
        return E_NOTIMPL;  // this build carries no thumbnail provider
#endif
#ifndef OZ_ENABLE_PREVIEW
    if (preview && !all)
        return E_NOTIMPL;  // this build carries no preview handler
#endif

    HRESULT hr = S_OK;
    if (bInstall)
        hr = RegisterAll(thumbnail, preview);
    else
        UnregisterAll(thumbnail, preview);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return hr;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    ClassFactory::Creator creator = nullptr;
    ozimg::Format format = ozimg::Format::Ozb;

    for (const FormatEntry& entry : kFormats)
    {
#ifdef OZ_ENABLE_THUMBNAIL
        if (IsEqualCLSID(rclsid, *entry.thumbnailClsid))
        {
            creator = CreateOzThumbnailProvider;
            format = entry.format;
            break;
        }
#endif
#ifdef OZ_ENABLE_PREVIEW
        if (IsEqualCLSID(rclsid, *entry.previewClsid))
        {
            creator = CreateOzPreviewHandler;
            format = entry.format;
            break;
        }
#endif
    }

    if (!creator)
        return CLASS_E_CLASSNOTAVAILABLE;

    ClassFactory* factory = new (std::nothrow) ClassFactory(creator, format);
    if (!factory)
        return E_OUTOFMEMORY;

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hModule;
        // The preview handler creates windows, so thread notifications are of
        // no use to us either way.
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
