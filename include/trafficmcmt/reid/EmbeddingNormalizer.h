#pragma once

#include <span>
#include <string>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt::reid {

/// Applies an affine projection followed by L2 normalisation:
///
///     y = W * (x - mean),  y /= ||y||
///
/// `W` is `out_dim x in_dim` row-major, which covers plain L2 (identity),
/// whitening (`out_dim == in_dim`) and PCA (`out_dim < in_dim`). The
/// projection is loaded from a small binary artifact so that training and
/// inference cannot drift apart.
///
/// Artifact layout (little-endian, IEEE-754 floats):
///
///     char[8]  magic  "TMCMTWN1"
///     uint32   in_dim
///     uint32   out_dim
///     float    mean[in_dim]
///     float    matrix[out_dim * in_dim]   // row-major
class EmbeddingNormalizer {
public:
    /// Plain L2 normalisation without a projection.
    [[nodiscard]] static Result<EmbeddingNormalizer> Identity(int dim);

    /// Builds a projection from explicit parameters.
    [[nodiscard]] static Result<EmbeddingNormalizer> FromWhitening(std::vector<float> mean,
                                                                   std::vector<float> matrix,
                                                                   int out_dim);

    /// Reads an artifact written by `Save`.
    [[nodiscard]] static Result<EmbeddingNormalizer> Load(const std::string& path);

    /// Writes the artifact. Returns `IoError` when the file cannot be opened.
    [[nodiscard]] Status Save(const std::string& path) const;

    [[nodiscard]] Result<std::vector<float>> Apply(std::span<const float> values) const;

    /// Normalises and keeps `model_id`. The output dimension becomes
    /// `OutDim()`.
    [[nodiscard]] Result<Embedding> Apply(const Embedding& input) const;

    [[nodiscard]] int InDim() const noexcept {
        return m_inDim;
    }
    [[nodiscard]] int OutDim() const noexcept {
        return m_outDim;
    }
    [[nodiscard]] bool IsIdentity() const noexcept {
        return m_identity;
    }

private:
    EmbeddingNormalizer() = default;

    bool m_identity = false;
    int m_inDim = 0;
    int m_outDim = 0;
    std::vector<float> m_mean;
    std::vector<float> m_matrix;
};

} // namespace tmcmt::reid
