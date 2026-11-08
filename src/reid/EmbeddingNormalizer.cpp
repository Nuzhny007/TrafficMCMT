#include "trafficmcmt/reid/EmbeddingNormalizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace tmcmt::reid {
namespace {

constexpr std::array<char, 8> kMagic{'T', 'M', 'C', 'M', 'T', 'W', 'N', '1'};
constexpr std::uint32_t kMaxDim = 65535;

void WriteU32(std::ostream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{static_cast<char>(value & 0xFFu),
                                    static_cast<char>((value >> 8) & 0xFFu),
                                    static_cast<char>((value >> 16) & 0xFFu),
                                    static_cast<char>((value >> 24) & 0xFFu)};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool ReadU32(std::istream& in, std::uint32_t& value) {
    std::array<unsigned char, 4> bytes{};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

bool AreFinite(std::span<const float> values) {
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

bool AreFinite(const std::vector<float>& values) {
    return AreFinite(std::span<const float>(values.data(), values.size()));
}

} // namespace

Result<EmbeddingNormalizer> EmbeddingNormalizer::Identity(int dim) {
    if (dim <= 0 || static_cast<std::uint32_t>(dim) > kMaxDim) {
        return Status::Error(ErrorCode::InvalidArgument, "normalizer dimension must be in [1, 65535]");
    }
    EmbeddingNormalizer normalizer;
    normalizer.m_identity = true;
    normalizer.m_inDim = dim;
    normalizer.m_outDim = dim;
    return normalizer;
}

Result<EmbeddingNormalizer> EmbeddingNormalizer::FromWhitening(std::vector<float> mean,
                                                               std::vector<float> matrix,
                                                               int out_dim) {
    if (mean.empty() || out_dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "whitening requires a non-empty mean and out_dim > 0");
    }
    if (mean.size() > kMaxDim || static_cast<std::uint32_t>(out_dim) > kMaxDim) {
        return Status::Error(ErrorCode::InvalidArgument, "whitening dimension exceeds 65535");
    }
    const std::size_t in_dim = mean.size();
    if (matrix.size() != in_dim * static_cast<std::size_t>(out_dim)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "whitening matrix has " + std::to_string(matrix.size()) + " values, expected " +
                                 std::to_string(in_dim * static_cast<std::size_t>(out_dim)));
    }
    if (!AreFinite(mean) || !AreFinite(matrix)) {
        return Status::Error(ErrorCode::InvalidArgument, "whitening artifact contains a non-finite value");
    }

    EmbeddingNormalizer normalizer;
    normalizer.m_identity = false;
    normalizer.m_inDim = static_cast<int>(in_dim);
    normalizer.m_outDim = out_dim;
    normalizer.m_mean = std::move(mean);
    normalizer.m_matrix = std::move(matrix);
    return normalizer;
}

Result<EmbeddingNormalizer> EmbeddingNormalizer::Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Status::Error(ErrorCode::IoError, "cannot open whitening artifact '" + path + "'");
    }

    std::array<char, 8> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != kMagic) {
        return Status::Error(ErrorCode::ParseError,
                             "whitening artifact '" + path + "' has an unknown magic header");
    }

    std::uint32_t in_dim = 0;
    std::uint32_t out_dim = 0;
    if (!ReadU32(in, in_dim) || !ReadU32(in, out_dim)) {
        return Status::Error(ErrorCode::ParseError,
                             "whitening artifact '" + path + "' is truncated");
    }
    if (in_dim == 0 || out_dim == 0 || in_dim > kMaxDim || out_dim > kMaxDim) {
        return Status::Error(ErrorCode::ParseError,
                             "whitening artifact '" + path + "' has invalid dimensions");
    }

    std::vector<float> mean(in_dim);
    std::vector<float> matrix(static_cast<std::size_t>(out_dim) * in_dim);
    in.read(reinterpret_cast<char*>(mean.data()),
            static_cast<std::streamsize>(mean.size() * sizeof(float)));
    in.read(reinterpret_cast<char*>(matrix.data()),
            static_cast<std::streamsize>(matrix.size() * sizeof(float)));
    if (!in) {
        return Status::Error(ErrorCode::ParseError,
                             "whitening artifact '" + path + "' is truncated");
    }
    if (in.peek() != std::char_traits<char>::eof()) {
        return Status::Error(ErrorCode::ParseError,
                             "whitening artifact '" + path + "' has trailing data");
    }
    if (!AreFinite(mean) || !AreFinite(matrix)) {
        return Status::Error(ErrorCode::ParseError,
                             "whitening artifact '" + path + "' contains a non-finite value");
    }

    EmbeddingNormalizer normalizer;
    normalizer.m_identity = false;
    normalizer.m_inDim = static_cast<int>(in_dim);
    normalizer.m_outDim = static_cast<int>(out_dim);
    normalizer.m_mean = std::move(mean);
    normalizer.m_matrix = std::move(matrix);
    return normalizer;
}

Status EmbeddingNormalizer::Save(const std::string& path) const {
    if (m_inDim <= 0 || m_outDim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "normalizer is not initialised");
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Status::Error(ErrorCode::IoError, "cannot open '" + path + "' for writing");
    }

    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    WriteU32(out, static_cast<std::uint32_t>(m_inDim));
    WriteU32(out, static_cast<std::uint32_t>(m_outDim));

    if (m_identity) {
        const std::vector<float> mean(static_cast<std::size_t>(m_inDim), 0.f);
        std::vector<float> matrix(static_cast<std::size_t>(m_outDim) * m_inDim, 0.f);
        for (int row = 0; row < m_outDim; ++row) {
            matrix[static_cast<std::size_t>(row) * m_inDim + static_cast<std::size_t>(row)] = 1.f;
        }
        out.write(reinterpret_cast<const char*>(mean.data()),
                  static_cast<std::streamsize>(mean.size() * sizeof(float)));
        out.write(reinterpret_cast<const char*>(matrix.data()),
                  static_cast<std::streamsize>(matrix.size() * sizeof(float)));
    } else {
        out.write(reinterpret_cast<const char*>(m_mean.data()),
                  static_cast<std::streamsize>(m_mean.size() * sizeof(float)));
        out.write(reinterpret_cast<const char*>(m_matrix.data()),
                  static_cast<std::streamsize>(m_matrix.size() * sizeof(float)));
    }

    if (!out.good()) {
        return Status::Error(ErrorCode::IoError, "failed to write '" + path + "'");
    }
    return Status::Ok();
}

Result<std::vector<float>> EmbeddingNormalizer::Apply(std::span<const float> values) const {
    if (m_inDim <= 0 || m_outDim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "normalizer is not initialised");
    }
    if (values.size() != static_cast<std::size_t>(m_inDim)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "normalizer expects " + std::to_string(m_inDim) + " values, got " +
                                 std::to_string(values.size()));
    }
    if (!AreFinite(values)) {
        return Status::Error(ErrorCode::InvalidArgument, "embedding contains a non-finite value");
    }

    std::vector<float> projected(static_cast<std::size_t>(m_outDim));
    if (m_identity) {
        std::copy(values.begin(), values.end(), projected.begin());
    } else {
        for (int row = 0; row < m_outDim; ++row) {
            const float* weights = m_matrix.data() + static_cast<std::size_t>(row) * m_inDim;
            float sum = 0.f;
            for (int column = 0; column < m_inDim; ++column) {
                sum += weights[column] *
                       (values[static_cast<std::size_t>(column)] - m_mean[static_cast<std::size_t>(column)]);
            }
            projected[static_cast<std::size_t>(row)] = sum;
        }
    }

    float squared = 0.f;
    for (const float value : projected) {
        squared += value * value;
    }
    const float norm = std::sqrt(squared);
    if (!(norm > std::numeric_limits<float>::epsilon()) || !std::isfinite(norm)) {
        return Status::Error(ErrorCode::InvalidArgument, "cannot normalise a zero-length embedding");
    }
    const float inverse = 1.f / norm;
    for (float& value : projected) {
        value *= inverse;
    }
    return projected;
}

Result<Embedding> EmbeddingNormalizer::Apply(const Embedding& input) const {
    if (!input.IsValid()) {
        return Status::Error(ErrorCode::InvalidArgument, "input embedding is not valid");
    }
    auto values = Apply(std::span<const float>(input.values.data(), input.values.size()));
    if (!values) {
        return values.status();
    }

    Embedding output;
    output.values = std::move(values).value();
    output.model_id = input.model_id;
    output.dim = static_cast<std::uint16_t>(m_outDim);
    return output;
}

} // namespace tmcmt::reid
