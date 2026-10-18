#include "arbiter/errors.hpp"

namespace arbiter {

const char* to_cstr(Errc c) noexcept {
    switch (c) {
        case Errc::Ok: return "Ok";
        case Errc::InvalidArgument: return "InvalidArgument";
        case Errc::OutOfRange: return "OutOfRange";
        case Errc::NotFound: return "NotFound";
        case Errc::Duplicate: return "Duplicate";
        case Errc::CrossedRejected: return "CrossedRejected";
        case Errc::InsufficientLiquidity: return "InsufficientLiquidity";
        case Errc::ChecksumMismatch: return "ChecksumMismatch";
        case Errc::ProtocolInvalid: return "ProtocolInvalid";
        case Errc::SequenceGap: return "SequenceGap";
        case Errc::JournalCorrupt: return "JournalCorrupt";
        case Errc::SnapshotInvalid: return "SnapshotInvalid";
        case Errc::RiskRejected: return "RiskRejected";
        case Errc::RateLimited: return "RateLimited";
        case Errc::Capacity: return "Capacity";
        case Errc::Unsupported: return "Unsupported";
        case Errc::IoError: return "IoError";
        case Errc::ConfigInvalid: return "ConfigInvalid";
        case Errc::Internal: return "Internal";
    }
    return "?";
}

}  // namespace arbiter