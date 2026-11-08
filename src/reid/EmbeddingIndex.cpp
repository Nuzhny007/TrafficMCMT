#include "trafficmcmt/reid/EmbeddingIndex.h"

#include <memory>
#include <string>

#include "trafficmcmt/reid/BruteForceIndex.h"

#if defined(TMCMT_WITH_FAISS)
#include "trafficmcmt/reid/FaissIndex.h"
#endif

namespace tmcmt::reid {

bool IsFaissAvailable() noexcept {
#if defined(TMCMT_WITH_FAISS)
    return true;
#else
    return false;
#endif
}

Result<std::unique_ptr<IEmbeddingIndex>> CreateEmbeddingIndex(IndexKind kind,
                                                              EmbeddingIndexOptions options) {
    if (options.dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "reid index dimension must be > 0");
    }

    switch (kind) {
    case IndexKind::Brute:
        return std::unique_ptr<IEmbeddingIndex>(std::make_unique<BruteForceIndex>(options));
    case IndexKind::FaissFlat:
    case IndexKind::FaissIvfPq:
    case IndexKind::FaissHnsw:
#if defined(TMCMT_WITH_FAISS)
        return FaissIndex::Create(kind, options);
#else
        return Status::Error(ErrorCode::Unsupported,
                             std::string("index '") + ToString(kind) +
                                 "' requires a build with TRAFFICMCMT_WITH_FAISS=ON");
#endif
    }
    return Status::Error(ErrorCode::Unsupported, "unknown embedding index kind");
}

} // namespace tmcmt::reid
