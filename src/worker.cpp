// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "worker.h"

#include "winutil.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using json = nlohmann::json;
using makine::plugin::Result;

namespace live {

namespace {

constexpr auto kShutdownBudget = std::chrono::milliseconds(2500);
constexpr auto kReadTimeout = std::chrono::seconds(45);   // twice, within the 120 s action budget
constexpr size_t kHistorySize = 5;
constexpr const wchar_t* kStateFile = L"\\state.json";

std::string dump(const json& j)
{
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

Result message(std::string_view text)
{
    return Result::ok(dump({{"message", text}}));
}

std::string messageOf(const Result& r)
{
    const json j = json::parse(r.json, nullptr, false);
    if (j.is_object() && j.contains("message") && j["message"].is_string()) return j["message"].get<std::string>();
    return "hata kodu " + std::to_string(r.status);
}

int intOf(const json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : 0;
}

std::string sizeText(const Region& r)
{
    return std::to_string(r.width) + "×" + std::to_string(r.height);
}

uint64_t fnv1a(const std::vector<uint8_t>& data)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t b : data) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Lines trimmed, runs of blanks folded into one space, empty lines dropped: OCR
// spacing jitter does not count as new text, any changed character does.
std::string normalize(std::string_view raw)
{
    std::string out, line;
    bool blank = false;
    const auto endLine = [&] {
        if (line.empty()) return;
        if (!out.empty()) out += '\n';
        out += line;
        line.clear();
    };
    for (const char c : raw) {
        if (c == '\n' || c == '\r') {
            endLine();
            blank = false;
        } else if (c == ' ' || c == '\t') {
            blank = !line.empty();
        } else {
            if (blank) line += ' ';
            blank = false;
            line += c;
        }
    }
    endLine();
    return out;
}

} // namespace

// ── state.json ──────────────────────────────────────────────────────────────

std::optional<Region> loadRegion(const std::wstring& dataDir)
{
    const auto text = readFile(dataDir + kStateFile);
    if (!text) return std::nullopt;
    const json state = json::parse(*text, nullptr, false);
    if (!state.is_object() || !state.contains("region") || !state["region"].is_object()) return std::nullopt;
    const json& r = state["region"];
    const Region region{intOf(r, "x"), intOf(r, "y"), intOf(r, "width"), intOf(r, "height")};
    return region.valid() ? std::optional(region) : std::nullopt;
}

// ── TranslationCache ────────────────────────────────────────────────────────

const std::string* TranslationCache::find(const std::string& source)
{
    const auto it = m_index.find(source);
    if (it == m_index.end()) return nullptr;
    m_items.splice(m_items.begin(), m_items, it->second);
    return &it->second->second;
}

void TranslationCache::put(const std::string& source, std::string translation)
{
    if (const auto it = m_index.find(source); it != m_index.end()) {
        m_items.erase(it->second);
        m_index.erase(it);
    }
    m_items.emplace_front(source, std::move(translation));
    m_index[source] = m_items.begin();
    if (m_items.size() > kCapacity) {
        m_index.erase(m_items.back().first);
        m_items.pop_back();
    }
}

void TranslationCache::clear()
{
    m_items.clear();
    m_index.clear();
}

// ── Worker: host thread ─────────────────────────────────────────────────────

Worker::Worker(makine::plugin::Host host, std::wstring dataDir, std::unique_ptr<RapidOcr> ocr, Config config,
               std::optional<Region> region)
    : m_host(host)
    , m_dataDir(std::move(dataDir))
    , m_ocr(std::move(ocr))
    , m_config(std::make_shared<const Config>(std::move(config)))
    , m_region(region)
{
}

void Worker::launch()
{
    m_thread = std::thread([self = shared_from_this()] { self->run(); });
}

void Worker::shutdown()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    m_translator.cancel();
    if (m_thread.joinable()) {
        bool finished = false;
        {
            std::unique_lock lock(m_mutex);
            finished = m_cv.wait_for(lock, kShutdownBudget, [this] { return m_finished; });
        }
        if (finished) {
            m_thread.join();
        } else {
            // Inside OCR or a request; the host ends the process right after.
            m_thread.detach();
            m_host.warn("İş parçacığı zamanında durmadı; süreç kapanırken kesilecek.");
            m_host.emitEvent("overlay.hide", "{}");
        }
    }
    bool picking = false;
    {
        std::lock_guard lock(m_mutex);
        picking = m_picking;
    }
    // An open pick waits on the user: leave it to the process exit.
    if (m_picker.joinable()) picking ? m_picker.detach() : m_picker.join();
}

std::shared_ptr<const Config> Worker::config() const
{
    std::lock_guard lock(m_mutex);
    return m_config;
}

void Worker::setConfig(Config config)
{
    {
        std::lock_guard lock(m_mutex);
        m_config = std::make_shared<const Config>(std::move(config));
        m_wake = true;
    }
    m_cv.notify_all();
}

bool Worker::hasRegion() const
{
    std::lock_guard lock(m_mutex);
    return m_region.has_value();
}

Result Worker::start()
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_region) return Result::error(MAKINE_E_FAILED, "Önce okunacak bölgeyi seçin (Bölge seç).");
        m_running = true;
        m_wake = true;
    }
    m_cv.notify_all();
    return message("Canlı çeviri başladı.");
}

Result Worker::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_running = false;
        m_wake = true;
    }
    m_cv.notify_all();
    return message("Durduruldu.");
}

Result Worker::pickRegion()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_picking) return Result::error(MAKINE_E_BUSY, "Bölge seçimi zaten açık.");
        m_picking = true;
    }
    if (m_picker.joinable()) m_picker.join();   // the previous pick, already done
    m_picker = std::thread([self = shared_from_this()] { self->runPicker(); });
    return message("Ekranda okunacak bölgeyi seçin.");
}

Result Worker::readOnce()
{
    std::optional<Region> region;
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, kReadTimeout, [this] { return !m_picking; });   // an open pick decides
        region = m_region;
    }
    if (!region) return Result::error(MAKINE_E_FAILED, "Önce okunacak bölgeyi seçin (Bölge seç).");

    // The worker thread owns capture and OCR; run there and wait.
    auto done = std::make_shared<std::promise<Result>>();
    auto result = done->get_future();
    post([this, done, r = *region] {
        Frame frame;
        std::string text, error;
        if (!m_capture.grab(r, frame, error))
            return done->set_value(Result::error(MAKINE_E_FAILED, "Ekran okunamadı: " + error));
        if (!m_ocr->recognize(frame, text, error))
            return done->set_value(Result::error(MAKINE_E_FAILED, "OCR: " + error));
        text = normalize(text);
        done->set_value(Result::ok(dump({
            {"message", text.empty() ? "Bölgede metin bulunamadı." : text},
            {"text", text},
            {"region", {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}},
            {"captured", {{"width", frame.width}, {"height", frame.height}}},
        })));
    });
    if (result.wait_for(kReadTimeout) != std::future_status::ready)
        return Result::error(MAKINE_E_TIMEOUT, "Okuma zamanında bitmedi.");
    return result.get();
}

void Worker::post(std::function<void()> job)
{
    {
        std::lock_guard lock(m_mutex);
        m_jobs.push_back(std::move(job));
    }
    m_cv.notify_all();
}

bool Worker::stillWanted() const
{
    std::lock_guard lock(m_mutex);
    return m_running && !m_quit;
}

// ── Worker: picker thread ───────────────────────────────────────────────────

void Worker::runPicker()
{
    std::optional<Region> picked;
    try {
        // Blocks until the user draws a rectangle or cancels.
        const Result r = m_host.call("overlay.pickRegion");
        if (r.succeeded()) {
            const json j = json::parse(r.json, nullptr, false);
            const Region region{intOf(j, "x"), intOf(j, "y"), intOf(j, "width"), intOf(j, "height")};
            if (j.is_object() && region.valid()) picked = region;
        }
        if (picked) {
            const json state = {{"region", {{"x", picked->x}, {"y", picked->y},
                                            {"width", picked->width}, {"height", picked->height}}}};
            if (!writeFileAtomic(m_dataDir + kStateFile, dump(state)))
                m_host.warn("Bölge kaydedilemedi (state.json); bir sonraki açılışta yeniden seçin.");
            m_host.info("Bölge: " + std::to_string(picked->x) + "," + std::to_string(picked->y) + " "
                        + sizeText(*picked));
        } else if (r.status == MAKINE_E_DENIED) {
            m_host.info("Bölge seçimi iptal edildi.");
        } else {
            reportError("Bölge seçilemedi: " + messageOf(r));
        }
    } catch (const std::exception& e) {
        m_host.error(std::string("Bölge seçimi: ") + e.what());
    }
    {
        std::lock_guard lock(m_mutex);
        if (picked) m_region = picked;
        m_picking = false;
        m_wake = true;
    }
    m_cv.notify_all();
}

// ── Worker: worker thread ───────────────────────────────────────────────────

void Worker::run()
{
    // GDI then uses physical pixels, whatever the host process declares.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        if (!m_jobs.empty()) {
            auto job = std::move(m_jobs.front());
            m_jobs.pop_front();
            lock.unlock();
            try {
                job();
            } catch (const std::exception& e) {
                m_host.error(std::string("İş: ") + e.what());
            }
            lock.lock();
            continue;
        }
        const bool wanted = m_running;
        const auto cfg = m_config;
        const auto region = m_region;
        m_wake = false;
        lock.unlock();
        try {
            step(wanted, cfg, region);
        } catch (const std::exception& e) {
            reportError(std::string("Beklenmeyen hata: ") + e.what());
        }
        lock.lock();
        const auto woken = [this] { return m_quit || m_wake || !m_jobs.empty(); };
        if (m_active)
            m_cv.wait_for(lock, std::chrono::milliseconds(cfg->intervalMs), woken);
        else
            m_cv.wait(lock, woken);
    }
    lock.unlock();
    hideOverlay();
    {
        std::lock_guard guard(m_mutex);
        m_finished = true;
    }
    m_cv.notify_all();
}

void Worker::step(bool wanted, const std::shared_ptr<const Config>& cfg, const std::optional<Region>& region)
{
    if (cfg != m_seenConfig) {
        if (m_seenConfig && !m_seenConfig->sameTranslation(*cfg)) {
            m_cache.clear();
            m_history.clear();
            clearError();   // a new key or service may well work
        }
        m_seenConfig = cfg;
        m_lastHash = 0;   // redo the current text under the new settings
        m_lastText.clear();
    }
    if (wanted && region) {
        if (!m_active || *region != m_activeRegion) {
            if (!m_active) clearError();
            m_active = true;
            m_activeRegion = *region;
            m_lastHash = 0;
            m_lastText.clear();
        }
    } else if (m_active) {
        m_active = false;
        m_stopped = true;
        hideOverlay();
    }
    setStatus(m_active ? "Çalışıyor · " + sizeText(m_activeRegion)
              : !region ? std::string("Bölge seçilmedi")
                        : (m_stopped ? "Durduruldu · " : "Hazır · ") + sizeText(*region));
    if (m_active) tick(*cfg, m_activeRegion);
}

void Worker::tick(const Config& cfg, const Region& region)
{
    Frame frame;
    std::string error;
    if (!m_capture.grab(region, frame, error)) return reportError("Ekran okunamadı: " + error);
    const uint64_t hash = fnv1a(frame.bgra);
    if (hash == m_lastHash) return;   // same pixels, same text
    m_lastHash = hash;

    std::string text;
    if (!m_ocr->recognize(frame, text, error)) return reportError("OCR: " + error);
    text = normalize(text);
    if (text == m_lastText) return;   // the picture moved, the words did not
    m_lastText = text;
    if (text.empty()) return hideOverlay();

    // A failed translation is not retried until the text changes: no request per
    // tick against a bad key.
    std::string shown;
    if (const std::string* cached = m_cache.find(text)) {
        shown = *cached;
    } else if (auto translated = m_translator.translate(cfg, text, m_history, error)) {
        shown = std::move(*translated);
        m_cache.put(text, shown);
        m_history.emplace_back(text, shown);
        if (m_history.size() > kHistorySize) m_history.pop_front();
    } else {
        reportError(error);
        shown = text;   // the recognised text beats an empty overlay
    }
    if (stillWanted()) showOverlay(cfg, region, shown, text);   // a stop may have come during the request
}

void Worker::setStatus(const std::string& text)
{
    if (text == m_status) return;
    m_status = text;
    m_host.emitEvent("ui.setStatus", dump({{"text", text}}));
}

void Worker::showOverlay(const Config& cfg, const Region& region, const std::string& shown,
                         const std::string& source)
{
    json payload = {{"text", shown},
                    {"x", region.x}, {"y", region.y}, {"width", region.width}, {"height", region.height}};
    if (cfg.showSource && shown != source) payload["sourceText"] = source;
    m_host.emitEvent("overlay.show", dump(payload));
    m_overlayShown = true;
}

void Worker::hideOverlay()
{
    if (!m_overlayShown) return;
    m_host.emitEvent("overlay.hide", "{}");
    m_overlayShown = false;
}

void Worker::reportError(const std::string& message)
{
    {
        std::lock_guard lock(m_errorMutex);
        if (message == m_lastError) return;
        m_lastError = message;
    }
    m_host.error(message);
    m_host.emitEvent("ui.notify", dump({{"title", "Canlı çeviri"}, {"message", message}, {"kind", "error"}}));
}

void Worker::clearError()
{
    std::lock_guard lock(m_errorMutex);
    m_lastError.clear();
}

} // namespace live
