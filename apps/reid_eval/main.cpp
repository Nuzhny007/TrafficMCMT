#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Version.h"
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/reid/GalleryManager.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsageError = 2;
constexpr int kExitDataError = 3;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    std::string tracklets;
    std::string manifest;
    tmcmt::TrackletFileFormat format = tmcmt::TrackletFileFormat::Auto;
    std::size_t maxRank = 10;
    double minSimilarity = 0.0;
    bool json = false;
    bool showHelp = false;
    bool showVersion = false;
};

void PrintUsage(std::string_view program) {
    std::cout << "TrafficMCMT ReID evaluation " << tmcmt::kVersion << "\n\n"
              << "Usage: " << program << " --tracklets <file> [options]\n"
              << "       " << program << " --manifest <file> [options]\n\n"
              << "Options:\n"
              << "  --tracklets <path>    Tracklet stream (JSONL/protobuf). Chunks are split\n"
              << "                        by sequence parity: even -> gallery, odd -> query.\n"
              << "  --manifest <path>     JSONL split with explicit identity/split fields.\n"
              << "  --format <fmt>        auto|jsonl|protobuf (default: auto)\n"
              << "  --max-rank <n>        CMC depth (default: 10; rank 5/10 always reported)\n"
              << "  --min-similarity <x>  Drop candidates below this cosine similarity\n"
              << "  --json                Print a JSON report\n"
              << "  --version             Print version and exit\n"
              << "  -h, --help            Print this help and exit\n\n"
              << "Manifest record:\n"
              << "  {\"id\":1,\"identity\":\"track-7\",\"camera\":\"cam-1\",\"model_id\":1,\n"
              << "   \"split\":\"gallery|query\",\"embedding\":[0.1,0.2,...]}\n";
}

template <typename T>
bool ParseUnsigned(const std::string& text, T& out) {
    try {
        out = static_cast<T>(std::stoull(text));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ParseDouble(const std::string& text, double& out) {
    try {
        std::size_t consumed = 0;
        out = std::stod(text, &consumed);
        return consumed == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<Options> ParseArgs(int argc, char** argv, std::string& error) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto nextValue = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--tracklets") {
            auto value = nextValue("--tracklets");
            if (!value) {
                return std::nullopt;
            }
            options.tracklets = *value;
        } else if (arg == "--manifest") {
            auto value = nextValue("--manifest");
            if (!value) {
                return std::nullopt;
            }
            options.manifest = *value;
        } else if (arg == "--format") {
            auto value = nextValue("--format");
            if (!value) {
                return std::nullopt;
            }
            if (*value == "auto") {
                options.format = tmcmt::TrackletFileFormat::Auto;
            } else if (*value == "jsonl" || *value == "json") {
                options.format = tmcmt::TrackletFileFormat::Jsonl;
            } else if (*value == "protobuf" || *value == "pb" || *value == "bin") {
                options.format = tmcmt::TrackletFileFormat::Protobuf;
            } else {
                error = "unknown format '" + *value + "', expected auto|jsonl|protobuf";
                return std::nullopt;
            }
        } else if (arg == "--max-rank") {
            auto value = nextValue("--max-rank");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseUnsigned(*value, options.maxRank) || options.maxRank == 0) {
                error = "invalid value for --max-rank: " + *value;
                return std::nullopt;
            }
        } else if (arg == "--min-similarity") {
            auto value = nextValue("--min-similarity");
            if (!value) {
                return std::nullopt;
            }
            if (!ParseDouble(*value, options.minSimilarity)) {
                error = "invalid value for --min-similarity: " + *value;
                return std::nullopt;
            }
        } else {
            error = "unknown argument '" + arg + "'";
            return std::nullopt;
        }
    }

    if (!options.showHelp && !options.showVersion) {
        if (options.tracklets.empty() == options.manifest.empty()) {
            error = "exactly one of --tracklets or --manifest is required";
            return std::nullopt;
        }
    }
    return options;
}

// ---------------------------------------------------------------------------
// Minimal JSON reader (flat objects, strings, numbers, arrays)
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    [[nodiscard]] const JsonValue* Find(std::string_view key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto& [name, value] : object) {
            if (name == key) {
                return &value;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : m_text(text) {}

    bool Parse(JsonValue& out, std::string& error) {
        SkipWhitespace();
        if (!ParseValue(out)) {
            error = m_error.empty() ? "invalid JSON" : m_error;
            return false;
        }
        SkipWhitespace();
        if (m_position != m_text.size()) {
            error = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace() {
        while (m_position < m_text.size()) {
            const char c = m_text[m_position];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++m_position;
        }
    }

    bool ParseValue(JsonValue& out) {
        if (m_position >= m_text.size()) {
            m_error = "unexpected end of JSON";
            return false;
        }
        switch (m_text[m_position]) {
        case '{':
            return ParseObject(out);
        case '[':
            return ParseArray(out);
        case '"':
            out.type = JsonValue::Type::String;
            return ParseString(out.text);
        case 't':
            return ParseLiteral("true", out, true);
        case 'f':
            return ParseLiteral("false", out, false);
        case 'n':
            return ParseLiteral("null", out, false);
        default:
            return ParseNumber(out);
        }
    }

    bool ParseLiteral(std::string_view literal, JsonValue& out, bool boolean) {
        if (m_text.substr(m_position, literal.size()) != literal) {
            m_error = "invalid literal";
            return false;
        }
        m_position += literal.size();
        if (literal == "null") {
            out.type = JsonValue::Type::Null;
        } else {
            out.type = JsonValue::Type::Bool;
            out.boolean = boolean;
        }
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const char* begin = m_text.data() + m_position;
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin) {
            m_error = "invalid number";
            return false;
        }
        m_position += static_cast<std::size_t>(end - begin);
        out.type = JsonValue::Type::Number;
        out.number = value;
        return true;
    }

    bool ParseString(std::string& out) {
        if (m_position >= m_text.size() || m_text[m_position] != '"') {
            m_error = "expected string";
            return false;
        }
        ++m_position;
        out.clear();
        while (m_position < m_text.size()) {
            const char c = m_text[m_position++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_position >= m_text.size()) {
                m_error = "unterminated escape";
                return false;
            }
            const char escaped = m_text[m_position++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escaped);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
                if (m_position + 4 > m_text.size()) {
                    m_error = "truncated unicode escape";
                    return false;
                }
                try {
                    const unsigned code = static_cast<unsigned>(
                        std::stoul(std::string(m_text.substr(m_position, 4)), nullptr, 16));
                    m_position += 4;
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                } catch (const std::exception&) {
                    m_error = "invalid unicode escape";
                    return false;
                }
                break;
            default:
                m_error = "invalid escape sequence";
                return false;
            }
        }
        m_error = "unterminated string";
        return false;
    }

    bool ParseArray(JsonValue& out) {
        ++m_position; // '['
        out.type = JsonValue::Type::Array;
        SkipWhitespace();
        if (m_position < m_text.size() && m_text[m_position] == ']') {
            ++m_position;
            return true;
        }
        for (;;) {
            JsonValue element;
            SkipWhitespace();
            if (!ParseValue(element)) {
                return false;
            }
            out.array.push_back(std::move(element));
            SkipWhitespace();
            if (m_position >= m_text.size()) {
                m_error = "unterminated array";
                return false;
            }
            if (m_text[m_position] == ',') {
                ++m_position;
                continue;
            }
            if (m_text[m_position] == ']') {
                ++m_position;
                return true;
            }
            m_error = "expected ',' or ']' in array";
            return false;
        }
    }

    bool ParseObject(JsonValue& out) {
        ++m_position; // '{'
        out.type = JsonValue::Type::Object;
        SkipWhitespace();
        if (m_position < m_text.size() && m_text[m_position] == '}') {
            ++m_position;
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (m_position >= m_text.size() || m_text[m_position] != ':') {
                m_error = "expected ':' after object key";
                return false;
            }
            ++m_position;
            JsonValue value;
            SkipWhitespace();
            if (!ParseValue(value)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (m_position >= m_text.size()) {
                m_error = "unterminated object";
                return false;
            }
            if (m_text[m_position] == ',') {
                ++m_position;
                continue;
            }
            if (m_text[m_position] == '}') {
                ++m_position;
                return true;
            }
            m_error = "expected ',' or '}' in object";
            return false;
        }
    }

    std::string_view m_text;
    std::size_t m_position = 0;
    std::string m_error;
};

// ---------------------------------------------------------------------------
// Evaluation data
// ---------------------------------------------------------------------------

struct EvalItem {
    tmcmt::TrackletId id = 0;
    std::string identity;
    tmcmt::CameraId camera;
    std::uint32_t model_id = 0;
    bool gallery = false;
    std::vector<float> values;
};

struct Metrics {
    std::size_t gallery = 0;
    std::size_t queries = 0;
    std::size_t identities = 0;
    std::size_t evaluated = 0;
    std::size_t skipped = 0;
    std::size_t dim = 0;
    std::size_t maxRank = 0;
    double rank1 = 0.0;
    double rank5 = 0.0;
    double rank10 = 0.0;
    double mAP = 0.0;
    std::set<std::uint32_t> modelIds;
};

std::string IdentityKey(const tmcmt::Tracklet& tracklet) {
    return tracklet.session_id + '\x1f' + tracklet.camera + '\x1f' +
           std::to_string(tracklet.local_track_id);
}

bool LoadTracklets(const std::string& path, tmcmt::TrackletFileFormat format,
                   std::vector<tmcmt::Tracklet>& tracklets, std::string& error) {
    tmcmt::FileTrackletSource::Options sourceOptions;
    sourceOptions.format = format;
    sourceOptions.speed = 0.0;

    auto source = tmcmt::FileTrackletSource::Open(path, sourceOptions);
    if (!source) {
        error = source.status().ToString();
        return false;
    }

    tmcmt::Tracklet tracklet;
    for (;;) {
        auto next = source.value()->Next(tracklet);
        if (!next) {
            error = next.status().ToString();
            return false;
        }
        if (!next.value()) {
            break;
        }
        tracklets.push_back(tracklet);
    }
    return true;
}

bool SplitTracklets(const std::vector<tmcmt::Tracklet>& tracklets, std::vector<EvalItem>& gallery,
                    std::vector<EvalItem>& queries, std::string& error) {
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < tracklets.size(); ++i) {
        const tmcmt::Tracklet& tracklet = tracklets[i];
        if (!tracklet.embedding.IsValid()) {
            error = "tracklet " + std::to_string(tracklet.id) + " has no valid embedding";
            return false;
        }
        if (tracklet.camera.empty()) {
            error = "tracklet " + std::to_string(tracklet.id) + " has an empty camera id";
            return false;
        }
        groups[IdentityKey(tracklet)].push_back(i);
    }

    for (auto& [identity, indices] : groups) {
        std::sort(indices.begin(), indices.end(), [&](std::size_t lhs, std::size_t rhs) {
            if (tracklets[lhs].sequence != tracklets[rhs].sequence) {
                return tracklets[lhs].sequence < tracklets[rhs].sequence;
            }
            return tracklets[lhs].id < tracklets[rhs].id;
        });

        for (const std::size_t index : indices) {
            const tmcmt::Tracklet& tracklet = tracklets[index];
            EvalItem item;
            item.id = tracklet.id;
            item.identity = identity;
            item.camera = tracklet.camera;
            item.model_id = tracklet.embedding.model_id;
            item.values = tracklet.embedding.values;
            // Chunk parity keeps temporally adjacent chunks in different
            // splits, so the query never trivially matches itself.
            item.gallery = (tracklet.sequence % 2) == 0;
            (item.gallery ? gallery : queries).push_back(std::move(item));
        }
    }
    return true;
}

bool LoadManifest(const std::string& path, std::vector<EvalItem>& gallery,
                  std::vector<EvalItem>& queries, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot open manifest '" + path + "'";
        return false;
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        // Tolerate a UTF-8 BOM written by PowerShell/editors.
        if (lineNumber == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        JsonValue root;
        std::string parseError;
        JsonParser parser(line);
        if (!parser.Parse(root, parseError)) {
            error = "manifest line " + std::to_string(lineNumber) + ": " + parseError;
            return false;
        }
        if (root.type != JsonValue::Type::Object) {
            error = "manifest line " + std::to_string(lineNumber) + ": expected a JSON object";
            return false;
        }

        const JsonValue* idValue = root.Find("id");
        const JsonValue* identityValue = root.Find("identity");
        const JsonValue* cameraValue = root.Find("camera");
        const JsonValue* modelValue = root.Find("model_id");
        const JsonValue* splitValue = root.Find("split");
        const JsonValue* embeddingValue = root.Find("embedding");

        if (idValue == nullptr || idValue->type != JsonValue::Type::Number ||
            identityValue == nullptr || identityValue->type != JsonValue::Type::String ||
            splitValue == nullptr || splitValue->type != JsonValue::Type::String ||
            embeddingValue == nullptr || embeddingValue->type != JsonValue::Type::Array) {
            error = "manifest line " + std::to_string(lineNumber) +
                    ": requires id, identity, split and embedding";
            return false;
        }

        EvalItem item;
        item.id = static_cast<tmcmt::TrackletId>(idValue->number);
        item.identity = identityValue->text;
        if (cameraValue != nullptr && cameraValue->type == JsonValue::Type::String) {
            item.camera = cameraValue->text;
        }
        if (modelValue != nullptr && modelValue->type == JsonValue::Type::Number) {
            item.model_id = static_cast<std::uint32_t>(modelValue->number);
        }
        if (splitValue->text == "gallery") {
            item.gallery = true;
        } else if (splitValue->text == "query") {
            item.gallery = false;
        } else {
            error = "manifest line " + std::to_string(lineNumber) +
                    ": split must be 'gallery' or 'query'";
            return false;
        }

        item.values.reserve(embeddingValue->array.size());
        for (const JsonValue& value : embeddingValue->array) {
            if (value.type != JsonValue::Type::Number) {
                error = "manifest line " + std::to_string(lineNumber) +
                        ": embedding must contain numbers only";
                return false;
            }
            item.values.push_back(static_cast<float>(value.number));
        }
        if (item.values.empty()) {
            error = "manifest line " + std::to_string(lineNumber) + ": empty embedding";
            return false;
        }

        (item.gallery ? gallery : queries).push_back(std::move(item));
    }
    return true;
}

bool Evaluate(const std::vector<EvalItem>& galleryItems, const std::vector<EvalItem>& queryItems,
              const Options& options, Metrics& metrics, std::string& error) {
    metrics.gallery = galleryItems.size();
    metrics.queries = queryItems.size();
    metrics.maxRank = std::max<std::size_t>(options.maxRank, 10);

    if (galleryItems.empty()) {
        error = "gallery is empty";
        return false;
    }
    if (queryItems.empty()) {
        error = "query set is empty";
        return false;
    }

    metrics.dim = galleryItems.front().values.size();
    for (const EvalItem& item : galleryItems) {
        if (item.values.size() != metrics.dim) {
            error = "gallery embeddings have inconsistent dimensions";
            return false;
        }
    }
    for (const EvalItem& item : queryItems) {
        if (item.values.size() != metrics.dim) {
            error = "query embeddings have inconsistent dimensions";
            return false;
        }
    }

    tmcmt::reid::GalleryOptions galleryOptions;
    galleryOptions.index.dim = static_cast<int>(metrics.dim);
    auto gallery = tmcmt::reid::GalleryManager::Create(tmcmt::IndexKind::Brute, galleryOptions);
    if (!gallery) {
        error = gallery.status().ToString();
        return false;
    }

    std::unordered_map<tmcmt::TrackletId, std::string> identityById;
    std::unordered_map<std::string, std::size_t> relevantCounts;
    std::set<std::string> identities;
    for (const EvalItem& item : galleryItems) {
        tmcmt::reid::GalleryEntry entry;
        entry.id = item.id;
        entry.camera = item.camera.empty() ? "unknown" : item.camera;
        entry.model_id = item.model_id;
        entry.values = item.values;
        const auto status = gallery.value()->Add(entry);
        if (!status) {
            error = status.ToString();
            return false;
        }
        identityById[item.id] = item.identity;
        ++relevantCounts[item.identity];
        identities.insert(item.identity);
    }
    for (const EvalItem& item : queryItems) {
        identities.insert(item.identity);
        metrics.modelIds.insert(item.model_id);
    }

    std::size_t maxRelevant = 0;
    for (const auto& [identity, count] : relevantCounts) {
        maxRelevant = std::max(maxRelevant, count);
    }
    const std::size_t searchK = std::max<std::size_t>(
        1, std::min(galleryItems.size(), std::max(metrics.maxRank, maxRelevant)));

    std::vector<std::size_t> cmc(metrics.maxRank + 1, 0);
    double averagePrecisionSum = 0.0;

    for (const EvalItem& query : queryItems) {
        const auto relevant = relevantCounts.find(query.identity);
        const std::size_t relevantTotal = relevant == relevantCounts.end() ? 0 : relevant->second;
        if (relevantTotal == 0) {
            ++metrics.skipped;
            continue;
        }

        tmcmt::reid::GalleryQuery galleryQuery;
        galleryQuery.top_k = searchK;
        galleryQuery.exclude_same_camera = false;
        galleryQuery.exclude_id = query.id;
        galleryQuery.model_id = query.model_id;
        galleryQuery.min_similarity = static_cast<float>(options.minSimilarity);

        auto hits = gallery.value()->Search(query.values, galleryQuery);
        if (!hits) {
            error = hits.status().ToString();
            return false;
        }

        std::size_t firstRank = 0;
        std::size_t relevantSeen = 0;
        double precisionSum = 0.0;
        for (std::size_t i = 0; i < hits.value().size(); ++i) {
            const auto identity = identityById.find(hits.value()[i].id);
            if (identity == identityById.end() || identity->second != query.identity) {
                continue;
            }
            ++relevantSeen;
            precisionSum += static_cast<double>(relevantSeen) / static_cast<double>(i + 1);
            if (firstRank == 0) {
                firstRank = i + 1;
            }
            if (relevantSeen >= relevantTotal) {
                break;
            }
        }

        if (firstRank != 0) {
            for (std::size_t rank = firstRank; rank <= metrics.maxRank; ++rank) {
                ++cmc[rank];
            }
        }
        averagePrecisionSum += precisionSum / static_cast<double>(relevantTotal);
        ++metrics.evaluated;
    }

    metrics.identities = identities.size();
    if (metrics.evaluated == 0) {
        error = "no query has a matching gallery identity";
        return false;
    }

    const double denominator = static_cast<double>(metrics.evaluated);
    metrics.rank1 = static_cast<double>(cmc[1]) / denominator;
    metrics.rank5 = static_cast<double>(cmc[std::min<std::size_t>(5, metrics.maxRank)]) / denominator;
    metrics.rank10 = static_cast<double>(cmc[std::min<std::size_t>(10, metrics.maxRank)]) / denominator;
    metrics.mAP = averagePrecisionSum / denominator;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string parseError;
    std::optional<Options> parsed = ParseArgs(argc, argv, parseError);
    if (!parsed) {
        std::cerr << "error: " << parseError << "\n\n";
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_reid_eval");
        return kExitUsageError;
    }
    if (parsed->showHelp) {
        PrintUsage(argc > 0 ? argv[0] : "trafficmcmt_reid_eval");
        return kExitOk;
    }
    if (parsed->showVersion) {
        std::cout << "TrafficMCMT " << tmcmt::kVersion << '\n';
        return kExitOk;
    }

    const Options& options = *parsed;
    std::vector<EvalItem> galleryItems;
    std::vector<EvalItem> queryItems;
    std::string error;
    std::string mode;

    if (!options.tracklets.empty()) {
        mode = "chunk-split";
        std::vector<tmcmt::Tracklet> tracklets;
        if (!LoadTracklets(options.tracklets, options.format, tracklets, error)) {
            std::cerr << "error: " << error << '\n';
            return kExitDataError;
        }
        if (!SplitTracklets(tracklets, galleryItems, queryItems, error)) {
            std::cerr << "error: " << error << '\n';
            return kExitDataError;
        }
    } else {
        mode = "manifest";
        if (!LoadManifest(options.manifest, galleryItems, queryItems, error)) {
            std::cerr << "error: " << error << '\n';
            return kExitDataError;
        }
    }

    Metrics metrics;
    if (!Evaluate(galleryItems, queryItems, options, metrics, error)) {
        std::cerr << "error: " << error << '\n';
        return kExitDataError;
    }

    if (options.json) {
        std::cout << std::fixed << std::setprecision(4) << "{"
                  << "\"mode\":\"" << mode << "\","
                  << "\"gallery\":" << metrics.gallery << ","
                  << "\"queries\":" << metrics.queries << ","
                  << "\"evaluated\":" << metrics.evaluated << ","
                  << "\"skipped\":" << metrics.skipped << ","
                  << "\"identities\":" << metrics.identities << ","
                  << "\"dim\":" << metrics.dim << ","
                  << "\"max_rank\":" << metrics.maxRank << ","
                  << "\"rank_1\":" << metrics.rank1 << ","
                  << "\"rank_5\":" << metrics.rank5 << ","
                  << "\"rank_10\":" << metrics.rank10 << ","
                  << "\"mAP\":" << metrics.mAP << "}\n";
    } else {
        std::cout << std::fixed << std::setprecision(4)
                  << "mode: " << mode << '\n'
                  << "gallery: " << metrics.gallery << '\n'
                  << "queries: " << metrics.queries << " (evaluated " << metrics.evaluated
                  << ", skipped " << metrics.skipped << ")\n"
                  << "identities: " << metrics.identities << '\n'
                  << "dim: " << metrics.dim << '\n'
                  << "rank_1: " << metrics.rank1 << '\n'
                  << "rank_5: " << metrics.rank5 << '\n'
                  << "rank_10: " << metrics.rank10 << '\n'
                  << "mAP: " << metrics.mAP << '\n';
    }
    return kExitOk;
}
