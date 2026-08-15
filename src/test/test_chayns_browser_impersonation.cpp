#include <drogon/drogon_test.h>
#include <drogon/HttpRequest.h>

#include <infrastructure/provider/chayns/ChaynsBrowserImpersonation.h>

using drogon::HttpRequest;

DROGON_TEST(chaynsBrowser_DefaultProfilePoolNotEmpty)
{
    auto cfg = chayns_browser::loadConfigFrom(Json::Value(Json::objectValue));
    CHECK(cfg.enabled == true);
    CHECK(cfg.profiles.size() >= 3);
    CHECK(chayns_browser::findProfileById(cfg, "sidekick_edge_mac") != nullptr);
}

DROGON_TEST(chaynsBrowser_AccountBindingIsStable)
{
    chayns_browser::ImpersonationConfig cfg;
    cfg.enabled = true;
    cfg.perAccountProfile = true;
    cfg.profiles = chayns_browser::defaultProfilePool();
    chayns_browser::setConfigForTest(cfg);

    const auto& a1 = chayns_browser::selectProfile("u:alice@example.com");
    const auto& a2 = chayns_browser::selectProfile("u:alice@example.com");
    const auto& b1 = chayns_browser::selectProfile("u:bob@example.com");
    CHECK(a1.id == a2.id);
    CHECK(!a1.userAgent.empty());
    CHECK(!b1.userAgent.empty());
    CHECK(a1.secChUa.find("Chromium") != std::string::npos);
}

DROGON_TEST(chaynsBrowser_ApplyHeadersOnRequest)
{
    chayns_browser::ImpersonationConfig cfg;
    cfg.enabled = true;
    cfg.perAccountProfile = false;
    cfg.defaultProfileId = "sidekick_edge_mac";
    cfg.origin = "https://sidekick.ki";
    cfg.referer = "https://sidekick.ki/";
    cfg.profiles = chayns_browser::defaultProfilePool();
    chayns_browser::setConfigForTest(cfg);

    auto req = HttpRequest::newHttpRequest();
    chayns_browser::applyBrowserHeadersForAccount(req, "user-1", "PID-1");

    CHECK(req->getHeader("User-Agent").find("Edg/151") != std::string::npos);
    CHECK(req->getHeader("Origin") == "https://sidekick.ki");
    CHECK(req->getHeader("Referer") == "https://sidekick.ki/");
    CHECK(req->getHeader("sec-fetch-mode") == "cors");
    CHECK(req->getHeader("sec-fetch-site") == "cross-site");
    CHECK(!req->getHeader("sec-ch-ua").empty());
    CHECK(req->getHeader("sec-ch-ua-mobile") == "?0");
    CHECK(req->getHeader("Accept-Encoding").find("gzip") != std::string::npos);
}

DROGON_TEST(chaynsBrowser_PerRequestOriginAndRefererOverride)
{
    chayns_browser::ImpersonationConfig cfg;
    cfg.enabled = true;
    cfg.perAccountProfile = false;
    cfg.defaultProfileId = "sidekick_edge_mac";
    cfg.origin = "https://sidekick.ki";
    cfg.referer = "https://sidekick.ki/";
    cfg.profiles = chayns_browser::defaultProfilePool();
    chayns_browser::setConfigForTest(cfg);

    auto req = HttpRequest::newHttpRequest();
    chayns_browser::applyBrowserHeadersForAccount(
        req,
        "pro@example.com",
        "PID-PRO",
        "https://mein.sidekick.ki",
        "https://mein.sidekick.ki/");

    CHECK(req->getHeader("Origin") == "https://mein.sidekick.ki");
    CHECK(req->getHeader("Referer") == "https://mein.sidekick.ki/");
}

DROGON_TEST(chaynsBrowser_DisabledDoesNotSetHeaders)
{
    chayns_browser::ImpersonationConfig cfg;
    cfg.enabled = false;
    cfg.profiles = chayns_browser::defaultProfilePool();
    chayns_browser::setConfigForTest(cfg);

    auto req = HttpRequest::newHttpRequest();
    chayns_browser::applyBrowserHeaders(req, "u:x");
    CHECK(req->getHeader("User-Agent").empty());
    CHECK(req->getHeader("Origin").empty());
}
