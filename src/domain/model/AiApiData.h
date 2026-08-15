#pragma once

#include <domain/model/ProviderModelCatalog.h>

#include <optional>
#include <string>

namespace aiapi {

/**
 * Transport metadata needed by request normalization.
 *
 * This is deliberately a value object: the controller copies only the
 * headers that affect request semantics, so the use case never receives a
 * Drogon request or reaches back into HTTP state from a background worker.
 */
struct RequestHeaders {
    std::string requestId;
    std::string correlationId;
    std::string userAgent;
    std::string originator;
    std::string codexWindowId;
    std::string threadId;
    std::string sessionId;
    std::string sessionIdUnderscore;
    std::string conversationId;
    std::string conversationIdUnderscore;
    std::string authorization;
};

/**
 * Controller-to-use-case input.  `jsonBody` remains serialized at the domain
 * boundary so domain contracts do not acquire a JsonCpp dependency.
 */
struct GenerationInput {
    std::string provider;
    std::string jsonBody;
    // Copied route metadata lets the application select a registered module
    // without retaining a Drogon request in queued work.
    std::string method;
    std::string path;
    RequestHeaders headers;
};

/** A transport-neutral failure payload shared by submission and execution. */
struct Error {
    int httpStatus = 500;
    std::string type = "internal_error";
    std::string message = "Internal server error";
    std::string code;
    std::string detail;
    std::string providerCode;
    int retryAfterSeconds = 0;
};

struct GenerationResult {
    std::optional<Error> error;

    bool succeeded() const { return !error.has_value(); }
};

enum class SubmissionOutcome {
    Accepted,
    QueueFull,
    ShuttingDown,
    Stopped,
    InvalidRequest,
};

struct SubmissionResult {
    SubmissionOutcome outcome = SubmissionOutcome::Stopped;
    std::optional<Error> error;

    bool accepted() const { return outcome == SubmissionOutcome::Accepted; }
};

enum class ModelCatalogOutcome {
    Found,
    ProviderNotFound,
};

struct ModelCatalogResult {
    ModelCatalogOutcome outcome = ModelCatalogOutcome::ProviderNotFound;
    ProviderModelCatalog catalog;

    bool found() const { return outcome == ModelCatalogOutcome::Found; }
};

enum class StoredResponseOutcome {
    Found,
    NotFound,
    Corrupt,
};

struct StoredResponseResult {
    StoredResponseOutcome outcome = StoredResponseOutcome::NotFound;
    std::string jsonBody;

    bool found() const { return outcome == StoredResponseOutcome::Found; }
};

enum class DeleteResponseOutcome {
    Deleted,
    NotFound,
};

struct DeleteResponseResult {
    DeleteResponseOutcome outcome = DeleteResponseOutcome::NotFound;

    bool deleted() const { return outcome == DeleteResponseOutcome::Deleted; }
};

}  // namespace aiapi
