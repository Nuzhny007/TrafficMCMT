#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>

#include <opencv2/core.hpp>

#include "trafficmcmt/edge/ColorHistogramReIdExtractor.h"

namespace {

using tmcmt::ColorHistogramOptions;
using tmcmt::ColorHistogramReIdExtractor;

cv::Mat MakeHalfRedHalfBlue() {
    cv::Mat image(64, 128, CV_8UC3, cv::Scalar::all(0));
    image(cv::Rect(0, 0, 64, 64)).setTo(cv::Scalar(0, 0, 255));   // BGR red
    image(cv::Rect(64, 0, 64, 64)).setTo(cv::Scalar(255, 0, 0));  // BGR blue
    return image;
}

TEST(ColorHistogramReIdExtractor, ProducesUnitLengthDescriptor) {
    ColorHistogramReIdExtractor extractor;
    const cv::Mat image = MakeHalfRedHalfBlue();

    auto embedding = extractor.Extract(image, cv::Rect(0, 0, image.cols, image.rows));
    ASSERT_TRUE(embedding) << embedding.status().ToString();
    EXPECT_EQ(embedding.value().dim, 512);
    EXPECT_EQ(embedding.value().values.size(), 512U);
    EXPECT_EQ(embedding.value().model_id, 1U);
    EXPECT_NEAR(embedding.value().Norm(), 1.f, 1e-5f);
}

TEST(ColorHistogramReIdExtractor, IsDeterministic) {
    ColorHistogramReIdExtractor extractor;
    const cv::Mat image = MakeHalfRedHalfBlue();
    const cv::Rect roi(10, 10, 40, 40);

    auto first = extractor.Extract(image, roi);
    auto second = extractor.Extract(image, roi);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_EQ(first.value().values.size(), second.value().values.size());
    for (std::size_t i = 0; i < first.value().values.size(); ++i) {
        EXPECT_FLOAT_EQ(first.value().values[i], second.value().values[i]);
    }
}

TEST(ColorHistogramReIdExtractor, DistinguishesDifferentRegions) {
    ColorHistogramReIdExtractor extractor;
    const cv::Mat image = MakeHalfRedHalfBlue();

    auto red = extractor.Extract(image, cv::Rect(8, 8, 40, 40));
    auto blue = extractor.Extract(image, cv::Rect(80, 8, 40, 40));
    ASSERT_TRUE(red);
    ASSERT_TRUE(blue);

    float distance = 0.f;
    for (std::size_t i = 0; i < red.value().values.size(); ++i) {
        distance += std::fabs(red.value().values[i] - blue.value().values[i]);
    }
    EXPECT_GT(distance, 0.5f);
}

TEST(ColorHistogramReIdExtractor, RejectsTinyOrOutsideRoi) {
    ColorHistogramReIdExtractor extractor;
    const cv::Mat image = MakeHalfRedHalfBlue();

    auto tiny = extractor.Extract(image, cv::Rect(0, 0, 2, 2));
    EXPECT_FALSE(tiny);
    EXPECT_EQ(tiny.status().code(), tmcmt::ErrorCode::InvalidArgument);

    auto outside = extractor.Extract(image, cv::Rect(1000, 1000, 20, 20));
    EXPECT_FALSE(outside);
    EXPECT_EQ(outside.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(ColorHistogramReIdExtractor, AcceptsGrayscaleInput) {
    ColorHistogramReIdExtractor extractor;
    cv::Mat gray(64, 64, CV_8UC1, cv::Scalar(128));

    auto embedding = extractor.Extract(gray, cv::Rect(0, 0, 64, 64));
    ASSERT_TRUE(embedding) << embedding.status().ToString();
    EXPECT_EQ(embedding.value().dim, 512);
    EXPECT_NEAR(embedding.value().Norm(), 1.f, 1e-5f);
}

TEST(ColorHistogramReIdExtractor, CustomBinsChangeDimension) {
    ColorHistogramOptions options;
    options.bins = 4;
    ColorHistogramReIdExtractor extractor(options);

    EXPECT_EQ(extractor.Dim(), 64);
    const cv::Mat image = MakeHalfRedHalfBlue();
    auto embedding = extractor.Extract(image, cv::Rect(0, 0, 32, 32));
    ASSERT_TRUE(embedding);
    EXPECT_EQ(embedding.value().dim, 64);
}

} // namespace
