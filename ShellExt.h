// Shared declarations: the DLL-wide reference count and the factory entry
// points for the two handler kinds.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#pragma once

// Which handlers are compiled in. A plain build has both; define exactly one of
// these to get a DLL that only carries that handler. Choosing what to *install*
// does not need a separate build - see DllInstall in ShellExt.cpp.
#if !defined(OZ_ENABLE_THUMBNAIL) && !defined(OZ_ENABLE_PREVIEW)
#define OZ_ENABLE_THUMBNAIL
#define OZ_ENABLE_PREVIEW
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#include "OzImage.h"

extern HINSTANCE g_hInst;
extern LONG g_cDllRef;

// Each handler is bound to one format at creation time: the CLSID the shell
// asked for says which extension it came from, so nothing has to be sniffed
// out of the file itself.
#ifdef OZ_ENABLE_THUMBNAIL
IUnknown* CreateOzThumbnailProvider(ozimg::Format format);
#endif
#ifdef OZ_ENABLE_PREVIEW
IUnknown* CreateOzPreviewHandler(ozimg::Format format);
#endif
