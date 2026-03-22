#pragma once

#include <json/json.h>
#include <optional>
#include <string>

enum class ManagedAccountKind
{
    ClassicProviderAccount,
    RetoolWorkspace
};

inline std::string managedAccountKindToString(ManagedAccountKind kind)
{
    switch (kind)
    {
        case ManagedAccountKind::ClassicProviderAccount:
            return "classic_provider_account";
        case ManagedAccountKind::RetoolWorkspace:
            return "retool_workspace";
        default:
            return "unknown";
    }
}

struct ManagedExecutionContext
{
    ManagedAccountKind kind = ManagedAccountKind::ClassicProviderAccount;
    std::string id;
    Json::Value data{Json::objectValue};
};

struct ManagedAccountRecord
{
    std::string id;
    ManagedAccountKind kind = ManagedAccountKind::ClassicProviderAccount;
    std::string provider;
    std::string displayName;
    std::string status;
    Json::Value metadata{Json::objectValue};

    Json::Value toJson() const
    {
        Json::Value value(Json::objectValue);
        value["id"] = id;
        value["kind"] = managedAccountKindToString(kind);
        value["provider"] = provider;
        value["displayName"] = displayName;
        value["status"] = status;
        value["metadata"] = metadata;
        return value;
    }
};
