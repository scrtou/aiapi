#include <drogon/drogon_test.h>

#include "utils/NexosUserAgent.h"

DROGON_TEST(NexosUserAgent_UsesSafeDefaultWhenUnsetOrInvalid)
{
    CHECK(nexos::resolveUserAgent(nullptr) == nexos::kDefaultUserAgent);
    CHECK(nexos::resolveUserAgent("") == nexos::kDefaultUserAgent);
    CHECK(nexos::resolveUserAgent(" \t \r\n") == nexos::kDefaultUserAgent);
    CHECK(nexos::resolveUserAgent("valid\r\nInjected: header") == nexos::kDefaultUserAgent);
}

DROGON_TEST(NexosUserAgent_UsesTrimmedConfiguredValue)
{
    CHECK(nexos::resolveUserAgent("  ExampleBrowser/1.0  ") == "ExampleBrowser/1.0");
}
