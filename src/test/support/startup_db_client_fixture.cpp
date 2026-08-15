#include <drogon/drogon.h>

#include <exception>
#include <iostream>

// Isolated regression fixture for Drogon's startup order.  ConfigLoader records
// db_clients immediately, but HttpAppFramework creates the actual clients only
// inside app().run(), before BeginningAdvice callbacks are invoked.
int main()
{
    Json::Value config(Json::objectValue);
    config["app"]["number_of_threads"] = 1;
    config["app"]["handle_sig_term"] = false;
    config["app"]["upload_path"] = "/tmp/aiapi-startup-db-client-fixture";

    Json::Value db(Json::objectValue);
    db["name"] = "startup_fixture_db";
    db["rdbms"] = "sqlite3";
    db["filename"] = ":memory:";
    db["number_of_connections"] = 1;
    db["is_fast"] = false;
    db["timeout"] = -1.0;
    config["db_clients"].append(db);

    bool dbClientReady = false;
    try {
        drogon::app().loadConfigJson(config);
        drogon::app().registerBeginningAdvice([&dbClientReady]() {
            dbClientReady = static_cast<bool>(
                drogon::app().getDbClient("startup_fixture_db"));
            std::cout << (dbClientReady ? "DB_CLIENT_READY" : "DB_CLIENT_MISSING")
                      << std::endl;
            drogon::app().quit();
        });
        drogon::app().run();
    } catch (const std::exception& ex) {
        std::cerr << "fixture exception: " << ex.what() << std::endl;
        return 2;
    }
    return dbClientReady ? 0 : 1;
}
