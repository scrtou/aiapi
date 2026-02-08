#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include "sessionManager/core/RequestAdapters.h"
using namespace drogon;

namespace {

HttpRequestPtr makeJsonRequest(const Json::Value& body,
                               const std::string& ua = "",
                               const std::string& auth = "") {
    auto req = HttpRequest::newHttpJsonRequest(body);
    if (!ua.empty()) {
        req->addHeader("user-agent", ua);
    }
    if (!auth.empty()) {
        req->addHeader("Authorization", auth);
    }
    return req;
}

}

DROGON_TEST(RequestAdapters_Chat_BasicFields)
{
    Json::Value body;
    body["model"] = "GPT-4o";
    body["stream"] = true;

    Json::Value msgs(Json::arrayValue);
    {
        Json::Value m;
        m["role"] = "system";
        m["content"] = "you are test";
        msgs.append(m);
    }
    {
        Json::Value m;
        m["role"] = "user";
        m["content"] = "hello";
        msgs.append(m);
    }
    body["messages"] = msgs;

    auto req = makeJsonRequest(body, "Kilo-Code/1.0", "Bearer test-key");
    auto genReq = RequestAdapters::buildGenerationRequestFromChat(req);

    CHECK(genReq.endpointType == EndpointType::ChatCompletions);
    CHECK(genReq.model == "GPT-4o");
    CHECK(genReq.stream);
    CHECK(genReq.systemPrompt.find("you are test") != std::string::npos);
    CHECK(genReq.currentInput.find("hello") != std::string::npos);
    CHECK(genReq.clientInfo["client_type"].asString() == "Kilo-Code");
    CHECK(genReq.clientInfo["client_authorization"].asString() == "test-key");
}

DROGON_TEST(RequestAdapters_Chat_ContentArrayWithImage)
{
    Json::Value body;
    body["model"] = "GPT-4o";

    Json::Value msgs(Json::arrayValue);
    Json::Value user;
    user["role"] = "user";
    Json::Value content(Json::arrayValue);
    {
        Json::Value textPart;
        textPart["type"] = "text";
        textPart["text"] = "look this image";
        content.append(textPart);
    }
    {
        Json::Value imgPart;
        imgPart["type"] = "image_url";
        Json::Value imageUrl;
        imageUrl["url"] = "https://example.com/a.png";
        imgPart["image_url"] = imageUrl;
        content.append(imgPart);
    }
    user["content"] = content;
    msgs.append(user);
    body["messages"] = msgs;

    auto req = makeJsonRequest(body);
    auto genReq = RequestAdapters::buildGenerationRequestFromChat(req);

    CHECK(genReq.currentInput.find("look this image") != std::string::npos);
    CHECK(genReq.images.size() == 1);
    CHECK(genReq.images[0].uploadedUrl == "https://example.com/a.png");
}

DROGON_TEST(RequestAdapters_Chat_ToolsAndToolChoice)
{
    Json::Value body;
    body["model"] = "GPT-4o";

    Json::Value msgs(Json::arrayValue);
    Json::Value user;
    user["role"] = "user";
    user["content"] = "do tool";
    msgs.append(user);
    body["messages"] = msgs;

    Json::Value tools(Json::arrayValue);
    Json::Value t;
    t["type"] = "function";
    t["function"]["name"] = "read_file";
    tools.append(t);
    body["tools"] = tools;

    Json::Value choice;
    choice["type"] = "function";
    choice["function"]["name"] = "read_file";
    body["tool_choice"] = choice;

    auto req = makeJsonRequest(body);
    auto genReq = RequestAdapters::buildGenerationRequestFromChat(req);

    CHECK(genReq.tools.isArray());
    CHECK(genReq.tools.size() == 1);
    CHECK(genReq.toolChoice.find("read_file") != std::string::npos);
}

DROGON_TEST(RequestAdapters_Responses_StringInputAndPreviousResponse)
{
    Json::Value body;
    body["model"] = "GPT-4o-mini";
    body["instructions"] = "keep short";
    body["input"] = "new prompt";
    body["previous_response_id"] = "resp_123";

    auto req = makeJsonRequest(body, "RooCode/2.0");
    auto genReq = RequestAdapters::buildGenerationRequestFromResponses(req);

    CHECK(genReq.endpointType == EndpointType::Responses);
    CHECK(genReq.model == "GPT-4o-mini");
    CHECK(genReq.systemPrompt == "keep short");
    CHECK(genReq.currentInput.find("new prompt") != std::string::npos);
    CHECK(genReq.previousResponseId.has_value());
    CHECK(*genReq.previousResponseId == "resp_123");
    CHECK(genReq.clientInfo["client_type"].asString() == "RooCode");
}

DROGON_TEST(RequestAdapters_Responses_InputItems)
{
    Json::Value body;
    body["model"] = "GPT-4o";

    Json::Value inputItems(Json::arrayValue);
    Json::Value textItem;
    textItem["type"] = "input_text";
    textItem["text"] = "from input_items";
    inputItems.append(textItem);

    Json::Value imgItem;
    imgItem["type"] = "input_image";
    imgItem["image_url"] = "https://example.com/b.png";
    inputItems.append(imgItem);

    body["input_items"] = inputItems;

    auto req = makeJsonRequest(body);
    auto genReq = RequestAdapters::buildGenerationRequestFromResponses(req);

    CHECK(genReq.currentInput.find("from input_items") != std::string::npos);
    CHECK(genReq.images.size() == 1);
    CHECK(genReq.images[0].uploadedUrl == "https://example.com/b.png");
    CHECK(!genReq.continuityTexts.empty());
}
