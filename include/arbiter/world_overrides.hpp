#pragma once
#include "arbiter/world_config.hpp"
#include <string>
namespace arbiter {
// Applies one strict override; validate the assembled configuration afterward.
bool apply_world_override(WorldConfig&, const std::string& key,
                          const std::string& raw_value, std::string& error);
}
