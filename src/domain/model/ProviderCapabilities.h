#pragma once

namespace provider {

/** Explicit provider capabilities instead of provider-name conditionals. */
struct ProviderCapabilities {
    bool nativeToolCalls = false;
    bool upstreamHistory = false;
    bool supportsImages = false;
};

}  // namespace provider
