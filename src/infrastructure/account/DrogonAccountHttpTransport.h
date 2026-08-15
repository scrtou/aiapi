#pragma once

#include <accountManager/AccountHttpTransport.h>

#include <memory>

namespace account {

/** Construct the infrastructure adapter that maps account HTTP DTOs to Drogon. */
std::shared_ptr<IAccountHttpTransport> makeDrogonAccountHttpTransport();

}  // namespace account
