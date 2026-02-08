#include "LogController.h"
#include "ControllerUtils.h"
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

using namespace drogon;

void LogController::logsList(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_INFO << "[LogCtrl] 获取日志文件列表";

    Json::Value response(Json::arrayValue);
    std::string logDir = "logs";

    try {
        for (const auto& entry : std::filesystem::directory_iterator(logDir)) {
            if (entry.is_regular_file()) {
                Json::Value fileInfo;
                fileInfo["name"] = entry.path().filename().string();
                fileInfo["size"] = static_cast<Json::Int64>(entry.file_size());

                auto ftime = entry.last_write_time();
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                auto tt = std::chrono::system_clock::to_time_t(sctp);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
                fileInfo["modified"] = std::string(buf);

                response.append(fileInfo);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "[LogCtrl] 列出日志文件失败：" << e.what();
    }

    ctl::sendJson(callback, response);
}

void LogController::logsTail(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback)
{
    LOG_DEBUG << "[LogCtrl] 读取日志尾部内容";

    std::string fileName = req->getParameter("file");
    if (fileName.empty()) fileName = "aiapi.log";

    // 安全检查：防止路径遍历
    if (fileName.find("..") != std::string::npos ||
        fileName.find('/') != std::string::npos ||
        fileName.find('\\') != std::string::npos) {
        ctl::sendError(callback, k400BadRequest, "invalid_request_error", "Invalid file name");
        return;
    }

    int lines = 200;
    std::string linesStr = req->getParameter("lines");
    if (!linesStr.empty()) {
        try { lines = std::stoi(linesStr); } catch (...) {}
    }
    if (lines < 1) lines = 1;
    if (lines > 5000) lines = 5000;

    std::string keyword = req->getParameter("keyword");
    std::string level = req->getParameter("level");

    std::string filePath = "logs/" + fileName;

    std::ifstream file(filePath, std::ios::ate);
    if (!file.is_open()) {
        ctl::sendError(callback, k404NotFound, "not_found", "Log file not found: " + fileName);
        return;
    }

    // 从文件尾部读取
    std::streampos fileSize = file.tellg();
    std::vector<std::string> allLines;

    // 读取文件尾部足够多的内容
    size_t bufSize = std::min(static_cast<size_t>(fileSize), static_cast<size_t>(lines * 500));
    file.seekg(-static_cast<std::streamoff>(bufSize), std::ios::end);

    std::string line;
    // 跳过可能不完整的第一行
    if (file.tellg() != std::streampos(0)) {
        std::getline(file, line);
    }

    while (std::getline(file, line)) {
        // 日志级别过滤
        if (!level.empty() && level != "ALL") {
            if (line.find(level) == std::string::npos) continue;
        }
        // 关键词过滤
        if (!keyword.empty()) {
            if (line.find(keyword) == std::string::npos) continue;
        }
        allLines.push_back(line);
    }
    file.close();

    // 只保留最后 N 行
    int startIdx = 0;
    if (static_cast<int>(allLines.size()) > lines) {
        startIdx = static_cast<int>(allLines.size()) - lines;
    }

    Json::Value response;
    response["file"] = fileName;
    response["total_lines"] = static_cast<Json::Int64>(allLines.size());
    response["returned_lines"] = static_cast<Json::Int64>(allLines.size() - startIdx);

    Json::Value linesArray(Json::arrayValue);
    for (int i = startIdx; i < static_cast<int>(allLines.size()); ++i) {
        linesArray.append(allLines[i]);
    }
    response["lines"] = linesArray;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
    response["timestamp"] = std::string(buf);

    ctl::sendJson(callback, response);
}
