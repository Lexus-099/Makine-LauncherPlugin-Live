// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "translator.h"

#include "winutil.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <exception>
#include <mutex>
#include <optional>
#include <string>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

using json = nlohmann::json;

namespace live {

namespace {

constexpr size_t kMaxResponse = 4u << 20;

std::string languageName(const std::string& code)
{
    static const std::pair<const char*, const char*> names[] = {
        {"en", "English"}, {"tr", "Turkish"}, {"de", "German"},
        {"fr", "French"},  {"es", "Spanish"}, {"ru", "Russian"},
    };
    for (const auto& [c, name] : names)
        if (code == c) return name;
    return code;
}

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A pasted key often carries a newline; inside a header it would end the line.
std::string trimmed(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::string dump(const json& j)
{
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string networkError(DWORD code)
{
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "zaman aşımı";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "sunucu adı çözülemedi (internet bağlantısı var mı?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "sunucuya bağlanılamadı";
    case ERROR_WINHTTP_SECURE_FAILURE: return "güvenli bağlantı kurulamadı";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "iptal edildi";
    default: return "ağ hatası (WinHTTP " + std::to_string(code) + ")";
    }
}

// The service's own explanation, when the body carries one.
std::string serviceMessage(const std::string& body)
{
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return {};
    const auto it = j.find("error");
    if (it != j.end() && it->is_object() && it->contains("message") && (*it)["message"].is_string())
        return (*it)["message"].get<std::string>();
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    if (j.contains("message") && j["message"].is_string()) return j["message"].get<std::string>();
    return {};
}

std::string httpError(const std::string& service, DWORD status, const std::string& body)
{
    std::string what;
    switch (status) {
    case 400: what = "istek reddedildi"; break;
    case 401:
    case 403: what = "API anahtarı geçersiz ya da bu servise yetkisi yok"; break;
    case 404: what = "adres ya da model bulunamadı"; break;
    case 429: what = "istek sınırı ya da kota aşıldı"; break;
    case 456: what = "karakter kotası doldu"; break;
    default: what = status >= 500 ? "servis şu an yanıt vermiyor" : "beklenmeyen yanıt"; break;
    }
    std::string message = service + ": " + what + " (HTTP " + std::to_string(status) + ")";
    if (const std::string detail = serviceMessage(body); !detail.empty()) message += " — " + detail.substr(0, 200);
    return message;
}

} // namespace

Translator::Translator()
{
    // Automatic proxy follows the Windows proxy settings (8.1+); the default one
    // only knows `netsh winhttp`.
    m_session = WinHttpOpen(L"Makine-Live", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session)
        m_session = WinHttpOpen(L"Makine-Live", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session) return;
    WinHttpSetTimeouts(m_session, 5000, 5000, 10000, 20000);   // resolve, connect, send, receive
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(m_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // Windows 10: no TLS 1.3 in WinHTTP
        WinHttpSetOption(m_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
}

Translator::~Translator()
{
    if (m_session) WinHttpCloseHandle(m_session);
}

void Translator::cancel()
{
    std::lock_guard lock(m_mutex);
    m_cancelled = true;
    // Closing the request handle is how WinHTTP aborts a synchronous call in flight.
    if (m_request) WinHttpCloseHandle(m_request);
    m_request = nullptr;
}

std::optional<Translator::Response> Translator::post(const wchar_t* host, const wchar_t* path,
                                                     const std::wstring& headers, const std::string& body,
                                                     std::string& error)
{
    if (!m_session) {
        error = "WinHTTP açılamadı";
        return std::nullopt;
    }
    const HINTERNET connect = WinHttpConnect(m_session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    DWORD failure = request ? 0 : GetLastError();
    {
        std::lock_guard lock(m_mutex);
        if (m_cancelled && request) {
            WinHttpCloseHandle(request);
            request = nullptr;
            failure = ERROR_WINHTTP_OPERATION_CANCELLED;
        }
        m_request = request;
    }

    std::optional<Response> out;
    if (request) {
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
            && WinHttpReceiveResponse(request, nullptr)) {
            Response r;
            DWORD size = sizeof r.status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &size, WINHTTP_NO_HEADER_INDEX);
            char buf[8192];
            DWORD got = 0;
            while (r.body.size() < kMaxResponse && WinHttpReadData(request, buf, sizeof buf, &got) && got > 0)
                r.body.append(buf, got);
            out = std::move(r);
        } else {
            failure = GetLastError();
        }
    }
    {
        std::lock_guard lock(m_mutex);
        if (m_request) WinHttpCloseHandle(m_request);   // already closed if cancel() ran
        m_request = nullptr;
    }
    if (connect) WinHttpCloseHandle(connect);
    if (!out) error = networkError(failure);
    return out;
}

std::optional<std::string> Translator::translate(const Config& cfg, const std::string& text,
                                                 const History& history, std::string& error)
{
    const std::string key = trimmed(cfg.apiKey);
    if (key.empty()) {
        error = "API anahtarı girilmedi; eklenti ayarlarından ekleyin.";
        return std::nullopt;
    }

    std::string service;
    const wchar_t* host = nullptr;
    const wchar_t* path = nullptr;
    std::wstring headers = L"Content-Type: application/json\r\n";
    json body;
    json::json_pointer answer;
    if (cfg.provider == "deepl") {
        service = "DeepL";
        host = key.ends_with(":fx") ? L"api-free.deepl.com" : L"api.deepl.com";   // free keys end in :fx
        path = L"/v2/translate";
        headers += L"Authorization: DeepL-Auth-Key " + widen(key) + L"\r\n";
        // English needs a variant as a target.
        body = {{"text", json::array({text})},
                {"source_lang", upper(cfg.sourceLang)},
                {"target_lang", cfg.targetLang == "en" ? "EN-US" : upper(cfg.targetLang)}};
        answer = json::json_pointer("/translations/0/text");
    } else if (cfg.provider == "google") {
        service = "Google";
        host = L"translation.googleapis.com";
        path = L"/language/translate/v2";
        headers += L"X-goog-api-key: " + widen(key) + L"\r\n";   // a header, never the URL
        body = {{"q", text}, {"source", cfg.sourceLang}, {"target", cfg.targetLang}, {"format", "text"}};
        answer = json::json_pointer("/data/translations/0/translatedText");
    } else {
        service = "OpenAI";
        host = L"api.openai.com";
        path = L"/v1/chat/completions";
        headers += L"Authorization: Bearer " + widen(key) + L"\r\n";
        json messages = json::array();
        messages.push_back({{"role", "system"},
                            {"content", "You translate text from a video game from " + languageName(cfg.sourceLang)
                                            + " to " + languageName(cfg.targetLang)
                                            + ". Reply with the translation only: no quotes, notes or "
                                              "explanations. Keep line breaks, names, numbers and placeholders."}});
        // Earlier lines keep names and terms consistent.
        for (const auto& [source, translation] : history) {
            messages.push_back({{"role", "user"}, {"content", source}});
            messages.push_back({{"role", "assistant"}, {"content", translation}});
        }
        messages.push_back({{"role", "user"}, {"content", text}});
        const std::string model = trimmed(cfg.model);
        // No temperature and max_completion_tokens: accepted by every current chat model.
        body = {{"model", model.empty() ? Config{}.model : model},
                {"messages", messages},
                {"max_completion_tokens", 1000}};
        answer = json::json_pointer("/choices/0/message/content");
    }

    const auto response = post(host, path, headers, dump(body), error);
    if (!response) {
        error = service + ": " + error;
        return std::nullopt;
    }
    if (response->status != 200) {
        error = httpError(service, response->status, response->body);
        return std::nullopt;
    }
    try {
        std::string translated = trimmed(json::parse(response->body).at(answer).get<std::string>());
        if (!translated.empty()) return translated;
        error = service + ": boş çeviri döndü";
    } catch (const std::exception&) {
        error = service + ": yanıt okunamadı";
    }
    return std::nullopt;
}

} // namespace live
