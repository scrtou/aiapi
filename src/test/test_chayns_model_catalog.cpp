#include <drogon/drogon_test.h>

#include <apipoint/chaynsapi/ChaynsModelCatalog.h>
#include <apipoint/chaynsapi/ChaynsPollingPolicy.h>

namespace {

Json::Value makeModel(const std::string& name, const std::string& personId)
{
    Json::Value model(Json::objectValue);
    model["showName"] = name;
    model["personId"] = personId;
    return model;
}

}  // namespace

DROGON_TEST(ChaynsModelCatalog_ParsesProAndMetadata)
{
    Json::Value payload(Json::arrayValue);
    Json::Value model = makeModel("Claude Fable 5", "CAI-CLDF5");
    model["usedModel"] = 130;
    model["tobitId"] = Json::Int64(5483359);
    model["needSidekickPro"] = true;
    model["canHandleImages"] = true;
    model["canHandleFunctionCalling"] = true;
    model["canHandleGoogleSearch"] = true;
    model["canUseThinking"] = true;
    model["developer"] = "Anthropic";
    model["developerCountry"] = "USA";
    model["hostingProvider"] = "Anthropic";
    model["hostingCountry"] = "USA";
    model["knowledge"] = "2025-12-01T00:00:00Z";
    model["costIndicator"] = 84;
    model["supportedMimeTypes"].append(" Image/* ");
    model["supportedMimeTypes"].append("image/*");
    model["supportedMimeTypes"].append("TEXT/PLAIN; charset=utf-8");
    model["skills"]["coding"] = 62;
    payload.append(model);

    const auto parsed = chayns::parseModelCatalog(payload);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.catalog.byName.size() == 1);

    const auto& descriptor = parsed.catalog.byName.at("Claude Fable 5");
    CHECK(descriptor.requiresPro);
    CHECK(descriptor.personId == "CAI-CLDF5");
    CHECK(descriptor.usedModel == 130);
    CHECK(descriptor.tobitId == 5483359);
    CHECK(descriptor.supportedMimeTypes.size() == 2);
    CHECK(descriptor.skills.at("coding") == 62);
    CHECK(chayns::supportsMimeType(descriptor, "image/png"));
    CHECK(chayns::supportsMimeType(descriptor, "text/plain"));

    const auto& output = parsed.catalog.openAiResponse["data"][0];
    CHECK(output["id"].asString() == "Claude Fable 5");
    CHECK(output["owned_by"].asString() == "Anthropic");
    CHECK(output["created"].asInt64() == 0);
    CHECK(output["x_chayns"]["requires_sidekick_pro"].asBool());
    CHECK(output["x_chayns"]["capabilities"]["function_calling"].asBool());
    CHECK(output["x_chayns"]["cost_indicator"].asInt() == 84);
}

DROGON_TEST(ChaynsModelCatalog_MissingProMeansFree)
{
    Json::Value payload(Json::arrayValue);
    Json::Value model = makeModel("Gemini 2.5 Flash", "CAI-GM25F");
    model["canHandleImages"] = true;
    payload.append(model);

    const auto parsed = chayns::parseModelCatalog(payload);
    REQUIRE(parsed.valid);
    const auto& descriptor = parsed.catalog.byName.at("Gemini 2.5 Flash");
    CHECK(!descriptor.requiresPro);
    CHECK(descriptor.canHandleImages);
    CHECK(chayns::supportsMimeType(descriptor, "image/jpeg"));
}

DROGON_TEST(ChaynsModelCatalog_MalformedProFailsClosed)
{
    Json::Value payload(Json::arrayValue);
    Json::Value model = makeModel("Future Model", "CAI-FUTURE");
    model["needSidekickPro"] = "yes";
    payload.append(model);

    const auto parsed = chayns::parseModelCatalog(payload);
    REQUIRE(parsed.valid);
    CHECK(parsed.catalog.byName.at("Future Model").requiresPro);
    CHECK(!parsed.warnings.empty());
}

DROGON_TEST(ChaynsModelCatalog_SkipsInvalidAndDuplicateModels)
{
    Json::Value payload(Json::arrayValue);
    payload.append(makeModel("Valid", "CAI-1"));
    payload.append(makeModel("Valid", "CAI-2"));
    payload.append(makeModel("Missing Person", ""));
    payload.append("not-an-object");

    const auto parsed = chayns::parseModelCatalog(payload);
    REQUIRE(parsed.valid);
    CHECK(parsed.catalog.byName.size() == 1);
    CHECK(parsed.duplicates == 1);
    CHECK(parsed.skipped == 2);
}

DROGON_TEST(ChaynsModelCatalog_MimeMatching)
{
    CHECK(chayns::normalizeMimeType(" IMAGE/PNG; charset=binary ") == "image/png");
    CHECK(chayns::mimeTypeMatches("image/*", "image/webp"));
    CHECK(chayns::mimeTypeMatches("*/*", "application/pdf"));
    CHECK(!chayns::mimeTypeMatches("text/*", "image/png"));

    chayns::ModelDescriptor model;
    model.canHandleImages = true;
    model.supportedMimeTypes = {"text/*"};
    CHECK(chayns::supportsImageInput(model));
    CHECK(chayns::supportsMimeType(model, "image/png"));
    CHECK(!chayns::supportsMimeType(model, "image/svg+xml"));

    model.supportedMimeTypes = {"text/*", "image/png"};
    CHECK(chayns::supportsMimeType(model, "image/png"));
    CHECK(!chayns::supportsMimeType(model, "image/jpeg"));
}

DROGON_TEST(ChaynsModelCatalog_AggregatesImageCapabilityConflicts)
{
    Json::Value payload(Json::arrayValue);
    Json::Value conflict = makeModel("Image Flag With Text Mimes", "CAI-IMG-CONFLICT");
    conflict["canHandleImages"] = true;
    conflict["supportedMimeTypes"].append("text/*");
    conflict["supportedMimeTypes"].append("application/json");
    payload.append(conflict);

    Json::Value narrowed = makeModel("PNG Only", "CAI-PNG-ONLY");
    narrowed["canHandleImages"] = true;
    narrowed["supportedMimeTypes"].append("image/png");
    payload.append(narrowed);

    const auto parsed = chayns::parseModelCatalog(payload);
    REQUIRE(parsed.valid);
    REQUIRE(parsed.imageCapabilityConflictModels.size() == 1);
    CHECK(parsed.imageCapabilityConflictModels[0] == "Image Flag With Text Mimes");

    const auto& conflictDescriptor = parsed.catalog.byName.at("Image Flag With Text Mimes");
    CHECK(chayns::supportsMimeType(conflictDescriptor, "image/webp"));

    const auto& narrowedDescriptor = parsed.catalog.byName.at("PNG Only");
    CHECK(chayns::supportsMimeType(narrowedDescriptor, "image/png"));
    CHECK(!chayns::supportsMimeType(narrowedDescriptor, "image/webp"));
}

DROGON_TEST(ChaynsPollingPolicy_AdaptiveBackoff)
{
    using namespace std::chrono;
    CHECK(chayns::pollingDelayForElapsed(milliseconds(0)) == milliseconds(200));
    CHECK(chayns::pollingDelayForElapsed(milliseconds(2999)) == milliseconds(200));
    CHECK(chayns::pollingDelayForElapsed(seconds(3)) == milliseconds(500));
    CHECK(chayns::pollingDelayForElapsed(seconds(15)) == seconds(1));
    CHECK(chayns::pollingDelayForElapsed(seconds(60)) == seconds(2));
    CHECK(chayns::kRequestPollingDeadline == minutes(5));
    CHECK(chayns::kUpstreamRequestTimeoutSeconds > 0.0);
    CHECK(chayns::kUpstreamUploadTimeoutSeconds >=
          chayns::kUpstreamRequestTimeoutSeconds);
    CHECK(chayns::kUpstreamRequestTimeoutSeconds <
          duration_cast<duration<double>>(chayns::kRequestPollingDeadline).count());
}
