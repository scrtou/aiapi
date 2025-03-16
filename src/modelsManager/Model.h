#include <string>
#include <vector>

struct Ability {
    bool canHandleImages;
    bool canHandleJsonFormat;
    bool canCoBotHandleJsonFormat;
    bool canHandleFunctionCalling;
    bool canUseThinking;
    bool isImageGenerationModel;
    bool canHandleGoogleSearch;
    std::vector<std::string> supportedMimeTypes;
};

struct Model {
    std::string modelName;
    std::string showName;
    int tobitId;
    Ability abilities;
};