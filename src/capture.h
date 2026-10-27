// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include "config.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutputDuplication;

namespace live {

struct Frame {
    std::vector<uint8_t> bgra;   // top-down, 4 bytes per pixel
    int width = 0;
    int height = 0;
};

/// Screen capture: DXGI Desktop Duplication of the monitor that holds the region
/// (also sees borderless and most fullscreen games), BitBlt on the screen DC when
/// DXGI is unavailable or the region spans monitors. Not thread-safe; the worker
/// thread owns it.
class ScreenCapture {
public:
    ScreenCapture() = default;
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /// `region` in physical virtual-desktop pixels.
    bool grab(const Region& region, Frame& out, std::string& error);

private:
    bool grabDxgi(const Region& region, Frame& out);
    bool openOutput(const Region& region);
    void closeOutput();

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIOutputDuplication* m_dup = nullptr;
    ID3D11Texture2D* m_staging = nullptr;
    int m_stagingWidth = 0, m_stagingHeight = 0;
    RECT m_output{};               // desktop rectangle of the duplicated monitor
    ULONGLONG m_retryDxgiAt = 0;   // after a failed open, GDI until then

    // DXGI hands out a frame only when the screen changed; the last copy answers
    // for an unchanged screen.
    Frame m_last;
    Region m_lastRegion;
};

} // namespace live
