#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/io/FileTrackletSource.h"
#include "trafficmcmt/io/TrackletSinks.h"

#include "TrackletTestHelpers.h"

namespace {

using tmcmt::BinaryTrackletSink;
using tmcmt::FileTrackletSource;
using tmcmt::JsonlTrackletSink;
using tmcmt::TrackletFileFormat;

std::string TempPath(const std::string& name) {
    const std::filesystem::path path = std::filesystem::path(::testing::TempDir()) / ("tmcmt_" + name);
    std::filesystem::remove(path);
    return path.string();
}

TEST(FileTrackletSource, ReadsJsonlWrittenBySink) {
    const std::string path = TempPath("source_jsonl.jsonl");
    const tmcmt::Tracklet first = tmcmt::test::MakeTracklet("s", "cam-01", 1, 0);
    const tmcmt::Tracklet second = tmcmt::test::MakeTracklet("s", "cam-01", 1, 1);

    auto sink = JsonlTrackletSink::Open(path);
    ASSERT_TRUE(sink) << sink.status().ToString();
    ASSERT_TRUE(sink.value()->Write(first).ok());
    ASSERT_TRUE(sink.value()->Write(second).ok());
    ASSERT_TRUE(sink.value()->Flush().ok());

    FileTrackletSource::Options options;
    options.speed = 0.0;
    auto source = FileTrackletSource::Open(path, options);
    ASSERT_TRUE(source) << source.status().ToString();
    EXPECT_EQ(source.value()->Format(), TrackletFileFormat::Jsonl);

    tmcmt::Tracklet tracklet;
    auto next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    ASSERT_TRUE(next.value());
    EXPECT_EQ(tracklet.sequence, 0U);
    EXPECT_EQ(tracklet.session_id, "s");
    EXPECT_EQ(tracklet.camera, "cam-01");

    next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    ASSERT_TRUE(next.value());
    EXPECT_EQ(tracklet.sequence, 1U);

    next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    EXPECT_FALSE(next.value());

    source.value().reset();
    sink.value().reset();
    std::filesystem::remove(path);
}

TEST(FileTrackletSource, ReadsFramedProtobufAndPreservesEndFlag) {
    const std::string path = TempPath("source_binary.bin");
    tmcmt::Tracklet first = tmcmt::test::MakeTracklet("s", "cam-02", 3, 0);
    tmcmt::Tracklet second = tmcmt::test::MakeTracklet("s", "cam-02", 3, 1);
    second.is_end = true;

    auto sink = BinaryTrackletSink::Open(path);
    ASSERT_TRUE(sink) << sink.status().ToString();
    ASSERT_TRUE(sink.value()->Write(first).ok());
    ASSERT_TRUE(sink.value()->Write(second).ok());
    ASSERT_TRUE(sink.value()->Flush().ok());

    FileTrackletSource::Options options;
    options.speed = 0.0;
    auto source = FileTrackletSource::Open(path, options);
    ASSERT_TRUE(source) << source.status().ToString();
    EXPECT_EQ(source.value()->Format(), TrackletFileFormat::Protobuf);

    tmcmt::Tracklet tracklet;
    auto next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    ASSERT_TRUE(next.value());
    EXPECT_FALSE(tracklet.is_end);

    next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    ASSERT_TRUE(next.value());
    EXPECT_TRUE(tracklet.is_end);
    EXPECT_EQ(tracklet.sequence, 1U);

    next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    EXPECT_FALSE(next.value());

    source.value().reset();
    sink.value().reset();
    std::filesystem::remove(path);
}

TEST(FileTrackletSource, ReadsQuantizedProtobuf) {
    const std::string path = TempPath("source_quantized.bin");
    tmcmt::Tracklet tracklet = tmcmt::test::MakeTracklet();
    tracklet.embedding.values = {0.5f, 0.25f, -0.5f, -0.25f};

    auto sink = BinaryTrackletSink::Open(path, true);
    ASSERT_TRUE(sink) << sink.status().ToString();
    ASSERT_TRUE(sink.value()->Write(tracklet).ok());
    ASSERT_TRUE(sink.value()->Flush().ok());

    FileTrackletSource::Options options;
    options.speed = 0.0;
    auto source = FileTrackletSource::Open(path, options);
    ASSERT_TRUE(source) << source.status().ToString();

    tmcmt::Tracklet read;
    auto next = source.value()->Next(read);
    ASSERT_TRUE(next) << next.status().ToString();
    ASSERT_TRUE(next.value());
    ASSERT_EQ(read.embedding.values.size(), tracklet.embedding.values.size());
    for (std::size_t i = 0; i < read.embedding.values.size(); ++i) {
        EXPECT_NEAR(read.embedding.values[i], tracklet.embedding.values[i], 0.01f);
    }

    source.value().reset();
    sink.value().reset();
    std::filesystem::remove(path);
}

TEST(FileTrackletSource, ResetRewindsStream) {
    const std::string path = TempPath("source_reset.jsonl");
    auto sink = JsonlTrackletSink::Open(path);
    ASSERT_TRUE(sink);
    ASSERT_TRUE(sink.value()->Write(tmcmt::test::MakeTracklet()).ok());
    ASSERT_TRUE(sink.value()->Flush().ok());

    FileTrackletSource::Options options;
    options.speed = 0.0;
    auto source = FileTrackletSource::Open(path, options);
    ASSERT_TRUE(source);

    tmcmt::Tracklet tracklet;
    ASSERT_TRUE(source.value()->Next(tracklet));
    ASSERT_TRUE(source.value()->Reset().ok());

    auto next = source.value()->Next(tracklet);
    ASSERT_TRUE(next) << next.status().ToString();
    EXPECT_TRUE(next.value());

    source.value().reset();
    sink.value().reset();
    std::filesystem::remove(path);
}

TEST(FileTrackletSource, ReportsMissingFile) {
    auto source = FileTrackletSource::Open(TempPath("does_not_exist.jsonl"));
    EXPECT_FALSE(source);
    EXPECT_EQ(source.status().code(), tmcmt::ErrorCode::IoError);
}

TEST(FileTrackletSource, ReportsCorruptFramedData) {
    const std::string path = TempPath("corrupt.bin");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        // Varint length 100 followed by only three bytes.
        stream.put(static_cast<char>(100));
        stream.write("abc", 3);
    }

    FileTrackletSource::Options options;
    options.format = TrackletFileFormat::Protobuf;
    options.speed = 0.0;
    auto source = FileTrackletSource::Open(path, options);
    ASSERT_TRUE(source) << source.status().ToString();

    tmcmt::Tracklet tracklet;
    auto next = source.value()->Next(tracklet);
    EXPECT_FALSE(next);
    EXPECT_EQ(next.status().code(), tmcmt::ErrorCode::ParseError);

    source.value().reset();
    std::filesystem::remove(path);
}

} // namespace
