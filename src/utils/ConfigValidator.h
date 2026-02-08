#ifndef CONFIG_VALIDATOR_H
#define CONFIG_VALIDATOR_H

#include <json/json.h>
#include <string>
#include <vector>

class ConfigValidator {
public:
    struct ValidationResult {
        bool valid = true;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };

    static ValidationResult validate(const Json::Value& config);
};

#endif

