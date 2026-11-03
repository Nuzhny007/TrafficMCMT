#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <memory>

#include <opencv2/core.hpp>

#include "trafficmcmt/edge/TypeAwareReIdExtractor.h"

namespace {

using tmcmt::ObjectType;
using tmcmt::TypeAwareReIdExtractor;
using tmcmt::TypeAwareReIdOptions;

cv::Mat MakeImage() {
    cv::Mat image(96, 160, CV_8UC3, cv::Scalar(40, 40, 40));
    image(cv::Rect(0, 0, 80, 96)).setTo(cv::Scalar(0, 0, 220));   // red-ish
    image(cv::Rect(80, 0, 80, 96)).setTo(cv::Scalar(220, 0, 0));  // blue-ish
    return image;
}

std::unique_ptr<TypeAwareReIdExtractor> MakeFallbackOnly() {
    TypeAwareReIdOptions options;
    options.person_model_id = 2;
    options.vehicle_model_id = 3;
    options.fallback_model_id = 1;
    auto created = TypeAwareReIdExtractor::Create(options);
    EXPECT_TRUE(created) << created.status().ToString();
    return created ? std::move(created).value() : nullptr;
}

TEST(TypeAwareReIdExtractor, UsesSeparateModelIdsPerType) {
    auto extractor = MakeFallbackOnly();
    ASSERT_NE(extractor, nullptr);
    EXPECT_FALSE(extractor->HasPersonModel());
    EXPECT_FALSE(extractor->HasVehicleModel());

    EXPECT_EQ(extractor->ModelIdFor(ObjectType::Pedestrian), 1U);
    EXPECT_EQ(extractor->ModelIdFor(ObjectType::Car), 3U);
    EXPECT_EQ(extractor->ModelIdFor(ObjectType::Truck), 3U);
    EXPECT_EQ(extractor->ModelIdFor(ObjectType::Bus), 3U);
    EXPECT_EQ(extractor->ModelIdFor(ObjectType::Bike), 3U);
    EXPECT_EQ(extractor->ModelIdFor(ObjectType::Unknown), 1U);

    EXPECT_EQ(extractor->DimFor(ObjectType::Pedestrian), 512);
    EXPECT_EQ(extractor->DimFor(ObjectType::Car), 512);
}

TEST(TypeAwareReIdExtractor, DispatchesByType) {
    auto extractor = MakeFallbackOnly();
    ASSERT_NE(extractor, nullptr);
    const cv::Mat image = MakeImage();
    const cv::Rect roi(8, 8, 64, 64);

    auto person = extractor->Extract(image, roi, ObjectType::Pedestrian);
    ASSERT_TRUE(person) << person.status().ToString();
    EXPECT_EQ(person.value().model_id, 1U);
    EXPECT_NEAR(person.value().Norm(), 1.f, 1e-5f);

    auto vehicle = extractor->Extract(image, roi, ObjectType::Car);
    ASSERT_TRUE(vehicle) << vehicle.status().ToString();
    EXPECT_EQ(vehicle.value().model_id, 3U);
    EXPECT_NEAR(vehicle.value().Norm(), 1.f, 1e-5f);

    auto unknown = extractor->Extract(image, roi);
    ASSERT_TRUE(unknown) << unknown.status().ToString();
    EXPECT_EQ(unknown.value().model_id, 1U);
}

TEST(TypeAwareReIdExtractor, RejectsMissingModel) {
    TypeAwareReIdOptions options;
    options.person_model_path = "this/model/does/not/exist.onnx";
    auto created = TypeAwareReIdExtractor::Create(options);
    EXPECT_FALSE(created);
    EXPECT_EQ(created.status().code(), tmcmt::ErrorCode::NotFound);
}

TEST(TypeAwareReIdExtractor, RejectsTinyRoi) {
    auto extractor = MakeFallbackOnly();
    ASSERT_NE(extractor, nullptr);
    const cv::Mat image = MakeImage();

    auto embedding = extractor->Extract(image, cv::Rect(0, 0, 2, 2), ObjectType::Pedestrian);
    EXPECT_FALSE(embedding);
    EXPECT_EQ(embedding.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

} // namespace
