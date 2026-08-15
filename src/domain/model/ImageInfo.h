#ifndef DOMAIN_MODEL_IMAGEINFO_H
#define DOMAIN_MODEL_IMAGEINFO_H

#include <string>

/**
 * @brief 多模态图片输入的纯数据描述
 *
 * 从 application/generation/contracts/GenerationRequest.h 抽出，使 apipoint 等模块
 * 无需依赖 sessionManager 即可使用该类型。无行为、无一方依赖。
 */
struct ImageInfo {
    std::string base64Data;      // base64编码的图片数据
    std::string mediaType;       // 图片类型如 image/png, image/jpeg
    std::string uploadedUrl;     // 上传后的图片URL
    int width = 0;
    int height = 0;
};

#endif // DOMAIN_MODEL_IMAGEINFO_H
