#include "arbiter/risk.hpp"

namespace arbiter {

const char* to_cstr(RiskReason r) noexcept {
    switch (r) {
        case kRiskOk: return "Ok";
        case kRiskKillSwitch: return "KillSwitch";
        case kRiskPriceBounds: return "PriceBounds";
        case kRiskMaxOrderQty: return "MaxOrderQty";
        case kRiskMaxPosition: return "MaxPosition";
        case kRiskMaxNotional: return "MaxNotional";
        case kRiskRateLimit: return "RateLimit";
        case kRiskDuplicateId: return "DuplicateId";
    }
    return "?";
}

}  // namespace arbiter