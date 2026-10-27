// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>
#include <string>

namespace live {

namespace {

template <class T>
void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

POINT centerOf(const Region& r)
{
    return {r.x + r.width / 2, r.y + r.height / 2};
}

} // namespace

ScreenCapture::~ScreenCapture()
{
    closeOutput();
}

bool ScreenCapture::grab(const Region& region, Frame& out, std::string& error)
{
    if (!region.valid()) {
        error = "geçersiz bölge";
        return false;
    }
    if (grabDxgi(region, out)) return true;

    // BitBlt on the screen DC: virtual-desktop coordinates, physical pixels because
    // the worker thread is per-monitor DPI aware.
    const HDC screen = GetDC(nullptr);
    if (!screen) {
        error = "ekran aygıtı açılamadı";
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = region.width;
    bi.bmiHeader.biHeight = -region.height;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HDC mem = CreateCompatibleDC(screen);
    const HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (mem && bmp && bits) {
        const HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, region.width, region.height, screen, region.x, region.y, SRCCOPY) != 0;
        GdiFlush();
        if (ok) {
            const auto* p = static_cast<const uint8_t*>(bits);
            out.width = region.width;
            out.height = region.height;
            out.bgra.assign(p, p + static_cast<size_t>(region.width) * region.height * 4);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) error = "ekran görüntüsü alınamadı (BitBlt " + std::to_string(GetLastError()) + ")";
    return ok;
}

bool ScreenCapture::grabDxgi(const Region& region, Frame& out)
{
    if (!m_dup || !PtInRect(&m_output, centerOf(region))) {
        if (GetTickCount64() < m_retryDxgiAt || !openOutput(region)) return false;
    }
    RECT want{region.x, region.y, region.x + region.width, region.y + region.height};
    RECT clip{};
    // A region that leaves this monitor goes to GDI, which sees the whole desktop.
    if (!IntersectRect(&clip, &want, &m_output) || !EqualRect(&clip, &want)) return false;
    const int w = clip.right - clip.left;
    const int h = clip.bottom - clip.top;

    // A timeout means nothing changed on screen; a frame whose LastPresentTime is 0
    // carries no new desktop image (the first one after DuplicateOutput is all
    // zeros). Either way the last copy is still current, if there is one.
    const bool haveCopy = m_lastRegion == region && !m_last.bgra.empty();
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* resource = nullptr;
    HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;
    for (const UINT wait : {0u, 100u}) {   // without a copy, wait a little for a real frame
        if (wait && haveCopy) break;
        hr = m_dup->AcquireNextFrame(wait, &info, &resource);
        if (SUCCEEDED(hr) && info.LastPresentTime.QuadPart == 0) {
            release(resource);
            m_dup->ReleaseFrame();
            hr = DXGI_ERROR_WAIT_TIMEOUT;
        }
        if (hr != DXGI_ERROR_WAIT_TIMEOUT) break;
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!haveCopy) return false;   // static screen and no copy yet: GDI answers
        out = m_last;
        return true;
    }
    if (FAILED(hr)) {
        // DXGI_ERROR_ACCESS_LOST: mode change, UAC prompt, fullscreen switch.
        closeOutput();
        return false;
    }

    ID3D11Texture2D* desktop = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktop));
    release(resource);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{};
        desktop->GetDesc(&desc);
        if (!m_staging || m_stagingWidth != w || m_stagingHeight != h) {
            release(m_staging);
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = static_cast<UINT>(w);
            sd.Height = static_cast<UINT>(h);
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = desc.Format;   // B8G8R8A8 for IDXGIOutput1 duplication
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (SUCCEEDED(m_device->CreateTexture2D(&sd, nullptr, &m_staging))) {
                m_stagingWidth = w;
                m_stagingHeight = h;
            }
        }
        if (m_staging) {
            const D3D11_BOX box{static_cast<UINT>(clip.left - m_output.left),
                                static_cast<UINT>(clip.top - m_output.top), 0,
                                static_cast<UINT>(clip.right - m_output.left),
                                static_cast<UINT>(clip.bottom - m_output.top), 1};
            m_context->CopySubresourceRegion(m_staging, 0, 0, 0, 0, desktop, 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                const size_t row = static_cast<size_t>(w) * 4;
                m_last.width = w;
                m_last.height = h;
                m_last.bgra.resize(row * h);
                for (int y = 0; y < h; ++y)
                    std::memcpy(m_last.bgra.data() + y * row,
                                static_cast<const uint8_t*>(mapped.pData) + y * size_t(mapped.RowPitch), row);
                m_context->Unmap(m_staging, 0);
                m_lastRegion = region;
                out = m_last;
                ok = true;
            }
        }
        release(desktop);
    }
    m_dup->ReleaseFrame();
    return ok;
}

bool ScreenCapture::openOutput(const Region& region)
{
    closeOutput();
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        m_retryDxgiAt = GetTickCount64() + 5000;
        return false;
    }
    const POINT center = centerOf(region);
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !m_dup && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; !m_dup && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            // A rotated monitor's duplicated image is not rotated; BitBlt handles those.
            const bool usable = desc.AttachedToDesktop && PtInRect(&desc.DesktopCoordinates, center)
                                && (desc.Rotation == DXGI_MODE_ROTATION_IDENTITY
                                    || desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED);
            IDXGIOutput1* output1 = nullptr;
            if (usable
                && SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &m_device, nullptr, &m_context))
                && SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1)))
                && SUCCEEDED(output1->DuplicateOutput(m_device, &m_dup))) {
                m_output = desc.DesktopCoordinates;
            } else if (usable) {
                release(m_context);
                release(m_device);
            }
            release(output1);
            release(output);
        }
        release(adapter);
    }
    release(factory);
    if (!m_dup) m_retryDxgiAt = GetTickCount64() + 5000;
    return m_dup != nullptr;
}

void ScreenCapture::closeOutput()
{
    release(m_staging);
    m_stagingWidth = m_stagingHeight = 0;
    release(m_dup);
    release(m_context);
    release(m_device);
    m_last = {};
    m_lastRegion = {};
}

} // namespace live
