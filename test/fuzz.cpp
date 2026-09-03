// Fuzz harness for the decoders. Links OzImage.cpp directly, so building it
// with /fsanitize=address puts every read and write under AddressSanitizer.
//
//   fuzz.exe <corpus-dir> [iterations-per-file] [seed]
//
// The corpus is walked for .ozb, .ozj and .ozt files. Each one is decoded as
// is, then mutated over and over: header bytes rewritten, random bytes
// flipped, and the buffer truncated at random points. Decode is expected to
// either succeed or fail cleanly - never to read or write out of bounds.
//
// Copyright (c) 2026 Rafael Cossovan de França (navossoc). SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

#include <random>
#include <string>
#include <vector>

#include "../OzImage.h"

namespace
{
    struct Sample
    {
        std::wstring path;
        ozimg::Format format;
        std::vector<BYTE> bytes;
    };

    bool ReadFile(const std::wstring& path, std::vector<BYTE>& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
                  size.QuadPart < 64 * 1024 * 1024;
        if (ok)
        {
            out.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                 read == out.size();
        }
        CloseHandle(h);
        return ok;
    }

    bool FormatForExtension(const std::wstring& path, ozimg::Format& out)
    {
        const size_t dot = path.rfind(L'.');
        if (dot == std::wstring::npos)
            return false;
        const std::wstring ext = path.substr(dot);
        if (_wcsicmp(ext.c_str(), L".ozb") == 0) { out = ozimg::Format::Ozb; return true; }
        if (_wcsicmp(ext.c_str(), L".ozj") == 0) { out = ozimg::Format::Ozj; return true; }
        if (_wcsicmp(ext.c_str(), L".ozt") == 0) { out = ozimg::Format::Ozt; return true; }
        return false;
    }

    void Collect(const std::wstring& dir, std::vector<Sample>& out, size_t limit)
    {
        WIN32_FIND_DATAW find = {};
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &find);
        if (h == INVALID_HANDLE_VALUE)
            return;

        do
        {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..")
                continue;
            const std::wstring full = dir + L"\\" + name;

            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                Collect(full, out, limit);
            }
            else if (out.size() < limit)
            {
                Sample s;
                if (FormatForExtension(name, s.format) && ReadFile(full, s.bytes))
                {
                    s.path = full;
                    out.push_back(std::move(s));
                }
            }
        } while (out.size() < limit && FindNextFileW(h, &find));

        FindClose(h);
    }

    // Keeps the header plausible often enough to reach the pixel loops, while
    // still hitting the reject paths.
    void Mutate(std::vector<BYTE>& buf, ozimg::Format format, std::mt19937& rng)
    {
        if (buf.empty())
            return;

        const size_t prefix = ozimg::PrefixSize(format);
        std::uniform_int_distribution<int> pick(0, 99);
        std::uniform_int_distribution<int> byte(0, 255);

        const int kind = pick(rng);

        if (kind < 45 && buf.size() > prefix + 18)
        {
            // Rewrite a header field. This is where the parser decisions are.
            std::uniform_int_distribution<size_t> where(prefix, prefix + 17);
            const int count = 1 + (pick(rng) % 4);
            for (int i = 0; i < count; ++i)
                buf[where(rng)] = static_cast<BYTE>(byte(rng));
        }
        else if (kind < 75)
        {
            // Flip bytes anywhere.
            std::uniform_int_distribution<size_t> where(0, buf.size() - 1);
            const int count = 1 + (pick(rng) % 8);
            for (int i = 0; i < count; ++i)
                buf[where(rng)] = static_cast<BYTE>(byte(rng));
        }
        else
        {
            // Truncate. Short buffers are what overflow checks are made of.
            std::uniform_int_distribution<size_t> len(0, buf.size());
            buf.resize(len(rng));
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"usage: fuzz.exe <corpus-dir> [iterations-per-file] [seed]\n");
        return 1;
    }

    const int iterations = (argc > 2) ? _wtoi(argv[2]) : 500;
    const unsigned seed = (argc > 3) ? static_cast<unsigned>(_wtoi(argv[3])) : 1;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<Sample> corpus;
    Collect(argv[1], corpus, 4000);
    wprintf(L"corpus: %zu files\n", corpus.size());
    if (corpus.empty())
        return 1;

    std::mt19937 rng(seed);
    size_t decoded = 0, rejected = 0;

    // Pass 1: every file untouched, so a regression in the happy path shows up.
    for (const Sample& s : corpus)
    {
        ozimg::Image image;
        if (SUCCEEDED(ozimg::Decode(s.bytes.data(), s.bytes.size(), s.format, image)))
        {
            ++decoded;
        }
        else
        {
            ++rejected;
            wprintf(L"  rejected: %s (%zu bytes)\n", s.path.c_str(), s.bytes.size());
        }
    }
    wprintf(L"clean pass: %zu decoded, %zu rejected\n", decoded, rejected);

    // Pass 2: mutations.
    size_t runs = 0;
    decoded = rejected = 0;
    for (const Sample& s : corpus)
    {
        for (int i = 0; i < iterations; ++i)
        {
            std::vector<BYTE> buf = s.bytes;
            Mutate(buf, s.format, rng);
            ++runs;

            ozimg::Image image;
            const HRESULT hr = buf.empty()
                ? E_FAIL
                : ozimg::Decode(buf.data(), buf.size(), s.format, image);
            if (SUCCEEDED(hr))
            {
                ++decoded;
                // Touch the result the way the callers do, so a bad width or
                // height would be caught here too.
                if (image.rgba.size() !=
                    static_cast<size_t>(image.width) * image.height * 4)
                {
                    wprintf(L"INCONSISTENT: %s (%dx%d, %zu bytes)\n", s.path.c_str(),
                            image.width, image.height, image.rgba.size());
                    return 2;
                }
            }
            else
            {
                ++rejected;
            }
        }
    }

    wprintf(L"mutations: %zu runs, %zu decoded, %zu rejected\n", runs, decoded, rejected);
    wprintf(L"no out-of-bounds access reported\n");

    CoUninitialize();
    return 0;
}
