// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <makine/plugin/abi.h>

#include <windows.h>

#include <string>

namespace live {

struct Frame;

/**
 * OCR through the prebuilt RapidOcrOnnx.dll (1.2.3, C API: OcrInit / OcrDetect /
 * OcrGetLen / OcrGetResult / OcrDestroy; ONNX Runtime and OpenCV linked into it).
 *
 * That API reads its image from a file, so each frame goes through one BMP in the
 * data folder, overwritten every time. Its paths are char*, and the DLL widens the
 * ONNX model paths byte by byte (OcrUtils.cpp strToWstr), so a non-ASCII path
 * fails even under a UTF-8 code page. Every path handed to it is therefore a
 * relative ASCII name, opened from a current directory set with the wide API.
 *
 * Not thread-safe; after load() only the worker thread uses it, and it owns the
 * process's current directory.
 */
class RapidOcr {
public:
    RapidOcr() = default;
    ~RapidOcr();
    RapidOcr(const RapidOcr&) = delete;
    RapidOcr& operator=(const RapidOcr&) = delete;

    /// Loads <pluginDir>\bin\RapidOcrOnnx.dll and <pluginDir>\models\*. `workDir`
    /// holds the image file. On failure returns the status for initialize() and a
    /// message for the user in `error`.
    MakineStatus load(const std::wstring& pluginDir, const std::wstring& workDir, std::string& error);

    /// Text lines found in the frame, joined by '\n' (empty when there are none).
    bool recognize(const Frame& frame, std::string& text, std::string& error);

private:
    using InitFn = void* (*)(const char*, const char*, const char*, const char*, int);
    using DetectFn = char (*)(void*, const char*, const char*, void*);
    using GetLenFn = int (*)(void*);
    using GetResultFn = char (*)(void*, char*, int);
    using DestroyFn = void (*)(void*);

    HMODULE m_dll = nullptr;
    void* m_handle = nullptr;
    DetectFn m_detect = nullptr;
    GetLenFn m_getLen = nullptr;
    GetResultFn m_getResult = nullptr;
    DestroyFn m_destroy = nullptr;

    std::wstring m_workDir;   // holds the image file
};

} // namespace live
