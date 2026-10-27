// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include "capture.h"
#include "config.h"
#include "ocr_rapid.h"
#include "translator.h"

#include <makine/plugin/sdk.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace live {

/// The region saved in <dataDir>\state.json, if any.
std::optional<Region> loadRegion(const std::wstring& dataDir);

/// Least recently used translations, keyed by the whole source text.
class TranslationCache {
public:
    const std::string* find(const std::string& source);
    void put(const std::string& source, std::string translation);
    void clear();

private:
    static constexpr size_t kCapacity = 200;
    std::list<std::pair<std::string, std::string>> m_items;   // most recent first
    std::unordered_map<std::string, std::list<std::pair<std::string, std::string>>::iterator> m_index;
};

/**
 * Capture → OCR → translate → overlay, on one thread that owns all of it. The
 * host thread only flips flags, swaps the settings snapshot and queues jobs, so a
 * settings change or a stop never touches the engines mid-frame. A second short
 * thread waits for the user's region pick.
 *
 * Both threads hold a shared_ptr to the Worker: one that misses the shutdown
 * budget is detached and finishes on memory that is still alive.
 */
class Worker : public std::enable_shared_from_this<Worker> {
public:
    Worker(makine::plugin::Host host, std::wstring dataDir, std::unique_ptr<RapidOcr> ocr, Config config,
           std::optional<Region> region);

    void launch();
    /// Stops both threads within ~2.5 s of the host's 3 s shutdown budget.
    void shutdown();

    std::shared_ptr<const Config> config() const;
    void setConfig(Config config);
    bool hasRegion() const;

    makine::plugin::Result start();
    makine::plugin::Result stop();
    makine::plugin::Result pickRegion();   // returns at once; the pick finishes on its own thread
    makine::plugin::Result readOnce();     // OCR of the region, no translation

private:
    void run();
    void step(bool wanted, const std::shared_ptr<const Config>& cfg, const std::optional<Region>& region);
    void tick(const Config& cfg, const Region& region);
    void runPicker();
    void post(std::function<void()> job);
    bool stillWanted() const;

    // Worker thread only (reportError also from the picker).
    void setStatus(const std::string& text);
    void showOverlay(const Config& cfg, const Region& region, const std::string& shown, const std::string& source);
    void hideOverlay();
    void reportError(const std::string& message);
    void clearError();

    const makine::plugin::Host m_host;   // a copy: thread-safe and valid until the process ends
    const std::wstring m_dataDir;

    // Worker thread only.
    std::unique_ptr<RapidOcr> m_ocr;
    ScreenCapture m_capture;
    TranslationCache m_cache;
    History m_history;
    std::shared_ptr<const Config> m_seenConfig;
    Region m_activeRegion;
    bool m_active = false;
    bool m_stopped = false;
    bool m_overlayShown = false;
    uint64_t m_lastHash = 0;
    std::string m_lastText;
    std::string m_status;

    Translator m_translator;   // cancel() from the host thread

    std::mutex m_errorMutex;
    std::string m_lastError;   // one notification per distinct error and run

    // Shared, guarded by m_mutex.
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::shared_ptr<const Config> m_config;
    std::optional<Region> m_region;
    std::deque<std::function<void()>> m_jobs;
    bool m_running = false;
    bool m_picking = false;
    bool m_wake = false;
    bool m_quit = false;
    bool m_finished = false;

    // Host thread only.
    std::thread m_thread;
    std::thread m_picker;
};

} // namespace live
