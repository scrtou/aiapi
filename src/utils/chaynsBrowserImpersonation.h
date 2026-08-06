#pragma once

#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/HttpRequest.h>
#include <json/json.h>
#include <string>
#include <vector>

// Browser request fingerprint helpers for chayns/sidekick upstream calls.
// P0: apply a coherent browser header set on every outbound request.
// P1: bind a stable profile per account identity (userName/personId) from a small pool.
namespace chayns_browser {

inline Json::Value safeCustomConfig()
{
    try {
        // 读取 Drogon 已加载的 custom_config。不能在此处调用自身，否则首次初始化
        // browser profile 时会无限递归并以堆栈溢出结束。
        return drogon::app().getCustomConfig();
    } catch (...) {
        return Json::Value(Json::objectValue);
    }
}


struct BrowserProfile {
    std::string id;
    std::string userAgent;
    std::string secChUa;
    std::string secChUaMobile;    // "?0" / "?1"
    std::string secChUaPlatform;  // e.g. "\"macOS\""
    std::string acceptLanguage;
};

struct ImpersonationConfig {
    bool enabled = true;
    bool perAccountProfile = true;
    std::string origin = "https://sidekick.ki";
    std::string referer = "https://sidekick.ki/";
    std::string accept = "*/*";
    // Only advertise encodings we can reliably decode end-to-end.
    std::string acceptEncoding = "gzip, deflate";
    std::string secFetchDest = "empty";
    std::string secFetchMode = "cors";
    std::string secFetchSite = "cross-site";
    std::string defaultProfileId = "sidekick_edge_mac";
    std::string acceptLanguageFallback =
        "zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6";
    std::vector<BrowserProfile> profiles;
};

inline BrowserProfile makeSidekickEdgeMac() {
    BrowserProfile p;
    p.id = "sidekick_edge_mac";
    p.userAgent =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36 Edg/151.0.0.0";
    p.secChUa =
        "\"Not=A?Brand\";v=\"99\", \"Microsoft Edge\";v=\"151\", \"Chromium\";v=\"151\"";
    p.secChUaMobile = "?0";
    p.secChUaPlatform = "\"macOS\"";
    p.acceptLanguage = "zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6";
    return p;
}

inline BrowserProfile makeChromeWindows() {
    BrowserProfile p;
    p.id = "chrome_windows";
    p.userAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";
    p.secChUa =
        "\"Not=A?Brand\";v=\"99\", \"Google Chrome\";v=\"151\", \"Chromium\";v=\"151\"";
    p.secChUaMobile = "?0";
    p.secChUaPlatform = "\"Windows\"";
    p.acceptLanguage = "en-US,en;q=0.9";
    return p;
}

inline BrowserProfile makeChromeMac() {
    BrowserProfile p;
    p.id = "chrome_mac";
    p.userAgent =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";
    p.secChUa =
        "\"Not=A?Brand\";v=\"99\", \"Google Chrome\";v=\"151\", \"Chromium\";v=\"151\"";
    p.secChUaMobile = "?0";
    p.secChUaPlatform = "\"macOS\"";
    p.acceptLanguage = "zh-CN,zh;q=0.9,en;q=0.8";
    return p;
}

inline BrowserProfile makeEdgeWindows() {
    BrowserProfile p;
    p.id = "edge_windows";
    p.userAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36 Edg/151.0.0.0";
    p.secChUa =
        "\"Not=A?Brand\";v=\"99\", \"Microsoft Edge\";v=\"151\", \"Chromium\";v=\"151\"";
    p.secChUaMobile = "?0";
    p.secChUaPlatform = "\"Windows\"";
    p.acceptLanguage = "en-US,en;q=0.9,de;q=0.8";
    return p;
}

inline BrowserProfile makeChromeLinux() {
    BrowserProfile p;
    p.id = "chrome_linux";
    p.userAgent =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/151.0.0.0 Safari/537.36";
    p.secChUa =
        "\"Not=A?Brand\";v=\"99\", \"Google Chrome\";v=\"151\", \"Chromium\";v=\"151\"";
    p.secChUaMobile = "?0";
    p.secChUaPlatform = "\"Linux\"";
    p.acceptLanguage = "en-US,en;q=0.9";
    return p;
}

inline std::vector<BrowserProfile> defaultProfilePool() {
    return {
        makeSidekickEdgeMac(),
        makeChromeWindows(),
        makeChromeMac(),
        makeEdgeWindows(),
        makeChromeLinux(),
    };
}

inline bool isSafeHeaderValue(const std::string& value) {
    return value.find_first_of("\r\n") == std::string::npos;
}

inline std::string trimCopy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::string readStringField(const Json::Value& obj,
                                   const char* key,
                                   const std::string& fallback = "") {
    if (!obj.isObject() || !obj.isMember(key) || !obj[key].isString()) {
        return fallback;
    }
    std::string value = trimCopy(obj[key].asString());
    if (value.empty() || !isSafeHeaderValue(value)) {
        return fallback;
    }
    return value;
}

inline BrowserProfile profileFromJson(const Json::Value& obj, const BrowserProfile& fallback) {
    BrowserProfile p = fallback;
    if (!obj.isObject()) {
        return p;
    }
    p.id = readStringField(obj, "id", p.id);
    p.userAgent = readStringField(obj, "user_agent", p.userAgent);
    p.secChUa = readStringField(obj, "sec_ch_ua", p.secChUa);
    p.secChUaMobile = readStringField(obj, "sec_ch_ua_mobile", p.secChUaMobile);
    p.secChUaPlatform = readStringField(obj, "sec_ch_ua_platform", p.secChUaPlatform);
    p.acceptLanguage = readStringField(obj, "accept_language", p.acceptLanguage);
    return p;
}

inline ImpersonationConfig loadConfigFrom(const Json::Value& customConfig) {
    ImpersonationConfig cfg;
    cfg.profiles = defaultProfilePool();

    if (!customConfig.isObject()) {
        return cfg;
    }

    const Json::Value providers = customConfig.get("providers", Json::Value(Json::objectValue));
    const Json::Value chayns = providers.isObject()
                                   ? providers.get("chaynsapi", Json::Value(Json::objectValue))
                                   : Json::Value(Json::objectValue);
    const Json::Value bi = chayns.isObject()
                               ? chayns.get("browser_impersonation", Json::Value(Json::objectValue))
                               : Json::Value(Json::objectValue);
    if (!bi.isObject() || bi.empty()) {
        return cfg;
    }

    if (bi.isMember("enabled") && bi["enabled"].isBool()) {
        cfg.enabled = bi["enabled"].asBool();
    }
    if (bi.isMember("per_account_profile") && bi["per_account_profile"].isBool()) {
        cfg.perAccountProfile = bi["per_account_profile"].asBool();
    }
    cfg.origin = readStringField(bi, "origin", cfg.origin);
    cfg.referer = readStringField(bi, "referer", cfg.referer);
    cfg.accept = readStringField(bi, "accept", cfg.accept);
    cfg.acceptEncoding = readStringField(bi, "accept_encoding", cfg.acceptEncoding);
    cfg.secFetchDest = readStringField(bi, "sec_fetch_dest", cfg.secFetchDest);
    cfg.secFetchMode = readStringField(bi, "sec_fetch_mode", cfg.secFetchMode);
    cfg.secFetchSite = readStringField(bi, "sec_fetch_site", cfg.secFetchSite);
    cfg.defaultProfileId = readStringField(bi, "default_profile", cfg.defaultProfileId);
    cfg.acceptLanguageFallback = readStringField(bi, "accept_language", cfg.acceptLanguageFallback);

    if (bi.isMember("profiles") && bi["profiles"].isArray() && !bi["profiles"].empty()) {
        std::vector<BrowserProfile> parsed;
        parsed.reserve(bi["profiles"].size());
        for (const auto& item : bi["profiles"]) {
            parsed.push_back(profileFromJson(item, makeSidekickEdgeMac()));
        }
        if (!parsed.empty()) {
            cfg.profiles = std::move(parsed);
        }
    }

    return cfg;
}

inline ImpersonationConfig& configSingleton() {
    static ImpersonationConfig cfg = []() {
        try {
            return loadConfigFrom(safeCustomConfig());
        } catch (...) {
            return loadConfigFrom(Json::Value(Json::objectValue));
        }
    }();
    return cfg;
}

// Allow tests / explicit refresh to replace config.
inline void setConfigForTest(ImpersonationConfig cfg) {
    configSingleton() = std::move(cfg);
}

inline void reloadConfigFromDrogon() {
    try {
        configSingleton() = loadConfigFrom(safeCustomConfig());
    } catch (...) {
        // keep previous
    }
}

inline const BrowserProfile* findProfileById(const ImpersonationConfig& cfg, const std::string& id) {
    for (const auto& p : cfg.profiles) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

inline uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

inline const BrowserProfile& selectProfile(const std::string& accountKey) {
    auto& cfg = configSingleton();
    if (cfg.profiles.empty()) {
        static const BrowserProfile kFallback = makeSidekickEdgeMac();
        return kFallback;
    }

    if (!cfg.perAccountProfile || accountKey.empty()) {
        if (const auto* p = findProfileById(cfg, cfg.defaultProfileId)) {
            return *p;
        }
        return cfg.profiles.front();
    }

    // Stable in-process + cross-restart mapping: hash(accountKey) % pool.
    const size_t idx = static_cast<size_t>(fnv1a64(accountKey) % cfg.profiles.size());
    return cfg.profiles[idx];
}

inline std::string accountKeyFor(const std::string& userName, const std::string& personId) {
    if (!userName.empty()) {
        return "u:" + userName;
    }
    if (!personId.empty()) {
        return "p:" + personId;
    }
    return "";
}

inline void applyBrowserHeaders(const drogon::HttpRequestPtr& req,
                                const std::string& accountKey = "",
                                const std::string& originOverride = "",
                                const std::string& refererOverride = "") {
    if (!req) {
        return;
    }
    const auto& cfg = configSingleton();
    if (!cfg.enabled) {
        return;
    }

    const BrowserProfile& profile = selectProfile(accountKey);
    const std::string& lang =
        !profile.acceptLanguage.empty() ? profile.acceptLanguage : cfg.acceptLanguageFallback;
    const std::string& origin = originOverride.empty() ? cfg.origin : originOverride;
    const std::string& referer = refererOverride.empty() ? cfg.referer : refererOverride;

    auto setIf = [&](const char* name, const std::string& value) {
        if (!value.empty() && isSafeHeaderValue(value)) {
            req->addHeader(name, value);
        }
    };

    setIf("User-Agent", profile.userAgent);
    setIf("Origin", origin);
    setIf("Referer", referer);
    setIf("Accept", cfg.accept);
    setIf("Accept-Language", lang);
    setIf("Accept-Encoding", cfg.acceptEncoding);
    setIf("sec-ch-ua", profile.secChUa);
    setIf("sec-ch-ua-mobile", profile.secChUaMobile);
    setIf("sec-ch-ua-platform", profile.secChUaPlatform);
    setIf("sec-fetch-dest", cfg.secFetchDest);
    setIf("sec-fetch-mode", cfg.secFetchMode);
    setIf("sec-fetch-site", cfg.secFetchSite);
}

inline void applyBrowserHeadersForAccount(const drogon::HttpRequestPtr& req,
                                          const std::string& userName,
                                          const std::string& personId = "",
                                          const std::string& originOverride = "",
                                          const std::string& refererOverride = "") {
    applyBrowserHeaders(
        req, accountKeyFor(userName, personId), originOverride, refererOverride);
}

}  // namespace chayns_browser
