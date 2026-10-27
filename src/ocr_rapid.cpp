// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "ocr_rapid.h"

#include "capture.h"
#include "winutil.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace live {

namespace {

// Relative to the plugin folder. tools/fetch_ocr.py downloads these; keep the two
// lists in step.
constexpr const char* kRuntime = "bin\\RapidOcrOnnx.dll";
constexpr const char* kDetModel = "models\\en_PP-OCRv3_det_mobile.onnx";            // text detection
constexpr const char* kClsModel = "models\\ch_ppocr_mobile_v2.0_cls_mobile.onnx";   // text direction
constexpr const char* kRecModel = "models\\en_PP-OCRv4_rec_mobile.onnx";            // English recognition
constexpr const char* kKeys = "models\\en_dict.txt";                                // its character set
constexpr const char* kImageName = "ocr-input.bmp";                                 // in the data folder

// OCR_PARAM of OcrLiteCApi.h. RapidOcrOnnx replaces a zero field with its default.
struct OcrParam {
    int padding;
    int maxSideLen;
    float boxScoreThresh;
    float boxThresh;
    float unClipRatio;
    int doAngle;
    int mostAngle;
};

const std::string kReinstall = " Eklentiyi kaldırıp yeniden kurun.";

std::wstring currentDirectory()
{
    std::wstring dir(GetCurrentDirectoryW(0, nullptr), L'\0');
    dir.resize(GetCurrentDirectoryW(static_cast<DWORD>(dir.size()), dir.data()));
    return dir;
}

bool writeBmp(const std::wstring& path, const Frame& f)
{
    const uint32_t pixels = static_cast<uint32_t>(f.bgra.size());
    std::vector<uint8_t> file(54 + f.bgra.size());
    const auto put32 = [&](size_t at, uint32_t v) { std::memcpy(file.data() + at, &v, 4); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, 54 + pixels);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(f.width));
    put32(22, static_cast<uint32_t>(-f.height));   // negative: rows run top-down
    file[26] = 1;                                   // planes
    file[28] = 32;                                  // bits per pixel, BI_RGB
    put32(34, pixels);
    std::copy(f.bgra.begin(), f.bgra.end(), file.begin() + 54);
    return writeFile(path, file.data(), file.size());
}

} // namespace

RapidOcr::~RapidOcr()
{
    if (m_handle && m_destroy) m_destroy(m_handle);
    // No FreeLibrary: ONNX Runtime keeps a thread pool, and the host process ends
    // right after the plugin's shutdown anyway.
}

MakineStatus RapidOcr::load(const std::wstring& pluginDir, const std::wstring& workDir, std::string& error)
{
    for (const char* file : {kRuntime, kDetModel, kClsModel, kRecModel, kKeys}) {
        if (!fileExists(pluginDir + L"\\" + widen(file))) {
            error = (file == kRuntime ? "OCR motoru bulunamadı (" : "OCR modeli eksik (") + std::string(file) + ")."
                    + kReinstall;
            return MAKINE_E_NOT_FOUND;
        }
    }

    // Full path, and only the DLL's own folder plus System32 for its imports: no
    // PATH or current-directory lookup that another file could answer.
    const std::wstring dll = pluginDir + L"\\" + widen(kRuntime);
    m_dll = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m_dll) {
        error = "OCR motoru yüklenemedi (Windows hata kodu " + std::to_string(GetLastError()) + ")." + kReinstall;
        return MAKINE_E_FAILED;
    }
    const auto init = reinterpret_cast<InitFn>(reinterpret_cast<void*>(GetProcAddress(m_dll, "OcrInit")));
    m_detect = reinterpret_cast<DetectFn>(reinterpret_cast<void*>(GetProcAddress(m_dll, "OcrDetect")));
    m_getLen = reinterpret_cast<GetLenFn>(reinterpret_cast<void*>(GetProcAddress(m_dll, "OcrGetLen")));
    m_getResult = reinterpret_cast<GetResultFn>(reinterpret_cast<void*>(GetProcAddress(m_dll, "OcrGetResult")));
    m_destroy = reinterpret_cast<DestroyFn>(reinterpret_cast<void*>(GetProcAddress(m_dll, "OcrDestroy")));
    if (!init || !m_detect || !m_getLen || !m_getResult || !m_destroy) {
        error = "OCR motoru beklenen sürüm değil (RapidOcrOnnx C API eksik)." + kReinstall;
        return MAKINE_E_FAILED;
    }

    // Relative names from inside the plugin folder are ASCII wherever Makine is
    // installed (C:\Users\Şükrü\…); see the class comment.
    const std::wstring previous = currentDirectory();
    if (!SetCurrentDirectoryW(pluginDir.c_str())) {
        error = "Eklenti klasörüne erişilemedi (" + std::to_string(GetLastError()) + ").";
        return MAKINE_E_FAILED;
    }
    // Leave cores to the game: inter-op threads only; ONNX Runtime sizes the rest.
    const int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    m_handle = init(kDetModel, kClsModel, kRecModel, kKeys, threads);
    SetCurrentDirectoryW(previous.c_str());
    if (!m_handle) {
        error = "OCR motoru başlatılamadı.";
        return MAKINE_E_FAILED;
    }
    m_workDir = workDir;
    return MAKINE_OK;
}

bool RapidOcr::recognize(const Frame& frame, std::string& text, std::string& error)
{
    text.clear();
    if (!m_handle) {
        error = "OCR motoru hazır değil";
        return false;
    }
    if (frame.width <= 0 || frame.height <= 0
        || frame.bgra.size() != static_cast<size_t>(frame.width) * frame.height * 4) {
        error = "boş ekran görüntüsü";
        return false;
    }
    if (!writeBmp(m_workDir + L"\\" + widen(kImageName), frame)) {
        error = "OCR görüntüsü yazılamadı (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    // The image is opened by a relative name too. Only this thread uses the
    // current directory once the plugin runs.
    if (!SetCurrentDirectoryW(m_workDir.c_str())) {
        error = "eklenti veri klasörüne erişilemedi (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    OcrParam param{50, 1024, 0.5f, 0.3f, 1.6f, 1, 1};
    // FALSE means "no text": RapidOcrOnnx then keeps the previous result, so it is
    // not read.
    if (!m_detect(m_handle, "", kImageName, &param)) return true;
    const int len = m_getLen(m_handle);   // text size + 1
    if (len <= 1) return true;
    std::string buf(static_cast<size_t>(len), '\0');
    m_getResult(m_handle, buf.data(), len);
    buf.resize(std::strlen(buf.c_str()));
    text = std::move(buf);
    return true;
}

} // namespace live
