#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <zmq.h>

#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/io/TrackletCodec.h"
#include "trafficmcmt/net/MessageFramer.h"
#include "trafficmcmt/net/SequenceTracker.h"
#include "trafficmcmt/net/ZmqBroker.h"
#include "trafficmcmt/net/ZmqResultSink.h"
#include "trafficmcmt/net/ZmqTrackletPublisher.h"
#include "trafficmcmt/net/ZmqTrackletSource.h"

namespace {

using namespace std::chrono_literals;

using tmcmt::BBox;
using tmcmt::ErrorCode;
using tmcmt::GlobalIdMergeEvent;
using tmcmt::GlobalTrackUpdate;
using tmcmt::Observation;
using tmcmt::Timestamp;
using tmcmt::Tracklet;

using tmcmt::net::FramerOptions;
using tmcmt::net::MessageFramer;
using tmcmt::net::MessageKind;
using tmcmt::net::SequenceEventKind;
using tmcmt::net::SequenceTracker;
using tmcmt::net::ZmqBroker;
using tmcmt::net::ZmqBrokerOptions;
using tmcmt::net::ZmqPublisherOptions;
using tmcmt::net::ZmqResultSink;
using tmcmt::net::ZmqResultSinkOptions;
using tmcmt::net::ZmqSourceOptions;
using tmcmt::net::ZmqTrackletPublisher;
using tmcmt::net::ZmqTrackletSource;

Tracklet MakeTracklet(std::string session, std::string camera, std::uint64_t sequence,
                      std::uint64_t localTrackId = 1) {
    Tracklet tracklet;
    tracklet.session_id = std::move(session);
    tracklet.camera = std::move(camera);
    tracklet.local_track_id = localTrackId;
    tracklet.sequence = sequence;
    tracklet.begin = Timestamp(1'000);
    tracklet.end = Timestamp(1'100);
    tracklet.type = tmcmt::ObjectType::Car;

    Observation observation;
    observation.ts = Timestamp(1'000);
    observation.bbox = BBox{1.f, 2.f, 3.f, 4.f, 0.9f};
    observation.world.valid = false;
    tracklet.observations.push_back(observation);

    tracklet.embedding.dim = 4;
    tracklet.embedding.model_id = 1;
    tracklet.embedding.values = {0.5f, 0.5f, 0.5f, 0.5f};
    return tracklet;
}

ZmqSourceOptions SourceOptions(std::string endpoint) {
    ZmqSourceOptions options;
    options.endpoint = std::move(endpoint);
    options.bind = true;
    options.timeout_ms = 50;
    options.rcv_hwm = 200'000;
    options.reconnect_ivl_ms = 100;
    options.strict = false;
    return options;
}

ZmqPublisherOptions PublisherOptions(std::string endpoint) {
    ZmqPublisherOptions options;
    options.endpoint = std::move(endpoint);
    options.bind = false;
    options.snd_hwm = 200'000;
    options.linger_ms = 0;
    return options;
}

/// Publishes warm-up tracklets until the subscriber receives one. This avoids
/// sleeping on the PUB/SUB slow-joiner problem.
bool WaitForSubscription(ZmqTrackletSource& source, ZmqTrackletPublisher& publisher,
                         std::chrono::milliseconds budget = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::uint64_t attempt = 0;
    Tracklet received;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)publisher.Publish(MakeTracklet("warmup", "cam-warm", attempt, attempt));
        ++attempt;

        auto next = source.Next(received);
        if (!next) {
            if (next.status().code() == ErrorCode::Timeout) {
                continue;
            }
            return false;
        }
        if (next.value()) {
            return true;
        }
    }
    return false;
}

/// Reads until a single timeout occurs, so that the next assertion starts from
/// an empty queue.
void DrainUntilQuiet(ZmqTrackletSource& source) {
    Tracklet tracklet;
    for (int i = 0; i < 1000; ++i) {
        auto next = source.Next(tracklet);
        if (!next) {
            return; // timeout or terminal error
        }
        if (!next.value()) {
            return;
        }
    }
}

std::optional<Tracklet> ReadSession(ZmqTrackletSource& source, const std::string& session,
                                    std::chrono::milliseconds budget = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    Tracklet tracklet;
    while (std::chrono::steady_clock::now() < deadline) {
        auto next = source.Next(tracklet);
        if (!next) {
            if (next.status().code() == ErrorCode::Timeout) {
                continue;
            }
            return std::nullopt;
        }
        if (!next.value()) {
            return std::nullopt;
        }
        if (tracklet.session_id == session) {
            return tracklet;
        }
    }
    return std::nullopt;
}

std::size_t ReadSessionCount(ZmqTrackletSource& source, const std::string& session, std::size_t expected,
                             std::chrono::milliseconds budget = 30s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    Tracklet tracklet;
    std::size_t count = 0;
    while (count < expected && std::chrono::steady_clock::now() < deadline) {
        auto next = source.Next(tracklet);
        if (!next) {
            if (next.status().code() == ErrorCode::Timeout) {
                continue;
            }
            break;
        }
        if (!next.value()) {
            break;
        }
        if (tracklet.session_id == session) {
            ++count;
        }
    }
    return count;
}

/// Minimal raw SUB socket, used to verify the result sink without adding a
/// typed consumer to the library.
class RawSubscriber {
public:
    RawSubscriber(const std::string& endpoint, const std::string& prefix) {
        m_context = zmq_ctx_new();
        m_socket = zmq_socket(m_context, ZMQ_SUB);
        const int linger = 0;
        zmq_setsockopt(m_socket, ZMQ_LINGER, &linger, sizeof(linger));
        const int timeout = 500;
        zmq_setsockopt(m_socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
        zmq_setsockopt(m_socket, ZMQ_SUBSCRIBE, prefix.data(), prefix.size());
        zmq_connect(m_socket, endpoint.c_str());
    }

    ~RawSubscriber() {
        if (m_socket != nullptr) {
            zmq_close(m_socket);
        }
        if (m_context != nullptr) {
            zmq_ctx_term(m_context);
        }
    }

    RawSubscriber(const RawSubscriber&) = delete;
    RawSubscriber& operator=(const RawSubscriber&) = delete;

    bool Receive(std::string& topic, std::string& payload, int timeoutMs) {
        zmq_pollitem_t item{};
        item.socket = m_socket;
        item.events = ZMQ_POLLIN;
        if (zmq_poll(&item, 1, timeoutMs) <= 0) {
            return false;
        }

        zmq_msg_t message;
        zmq_msg_init(&message);
        if (zmq_msg_recv(&message, m_socket, 0) < 0) {
            zmq_msg_close(&message);
            return false;
        }
        topic.assign(static_cast<const char*>(zmq_msg_data(&message)), zmq_msg_size(&message));
        const bool more = zmq_msg_more(&message) != 0;
        zmq_msg_close(&message);
        if (!more) {
            return false;
        }

        zmq_msg_init(&message);
        if (zmq_msg_recv(&message, m_socket, 0) < 0) {
            zmq_msg_close(&message);
            return false;
        }
        payload.assign(static_cast<const char*>(zmq_msg_data(&message)), zmq_msg_size(&message));
        zmq_msg_close(&message);
        return true;
    }

private:
    void* m_context = nullptr;
    void* m_socket = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// MessageFramer
// ---------------------------------------------------------------------------

TEST(MessageFramer, BuildsAndParsesCameraTopics) {
    const MessageFramer framer;

    auto tracklet = framer.MakeTopic("cam-01", MessageKind::Tracklet);
    ASSERT_TRUE(tracklet);
    EXPECT_EQ(tracklet.value(), "tmcmt.v1.cam-01.tracklet");

    auto parsed = framer.ParseTopic(tracklet.value());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().version, 1u);
    EXPECT_EQ(parsed.value().camera_id, "cam-01");
    EXPECT_EQ(parsed.value().kind, MessageKind::Tracklet);

    auto status = framer.MakeTopic("cam-02", MessageKind::Status);
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value(), "tmcmt.v1.cam-02.status");
}

TEST(MessageFramer, BuildsAndParsesGlobalTopics) {
    const MessageFramer framer;

    auto track = framer.MakeTopic({}, MessageKind::GlobalTrack);
    ASSERT_TRUE(track);
    EXPECT_EQ(track.value(), "tmcmt.v1.global.track");

    auto parsed = framer.ParseTopic(track.value());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().kind, MessageKind::GlobalTrack);
    EXPECT_TRUE(parsed.value().camera_id.empty());

    auto calib = framer.MakeTopic({}, MessageKind::Calibration);
    ASSERT_TRUE(calib);
    EXPECT_EQ(calib.value(), "tmcmt.v1.calib");
    auto parsedCalib = framer.ParseTopic(calib.value());
    ASSERT_TRUE(parsedCalib);
    EXPECT_EQ(parsedCalib.value().kind, MessageKind::Calibration);
}

TEST(MessageFramer, RejectsForeignVersionAndMalformedTopics) {
    const MessageFramer framer;

    EXPECT_FALSE(framer.ParseTopic("tmcmt.v2.cam-01.tracklet"));
    EXPECT_FALSE(framer.ParseTopic("tmcmt.v1.cam-01.unknown"));
    EXPECT_FALSE(framer.ParseTopic("tmcmt.v1..tracklet"));
    EXPECT_FALSE(framer.ParseTopic("tmcmt.v1.cam.01.tracklet"));
    EXPECT_FALSE(framer.ParseTopic("other.v1.cam-01.tracklet"));

    EXPECT_FALSE(framer.MakeTopic("", MessageKind::Tracklet));
    EXPECT_FALSE(framer.MakeTopic("cam.01", MessageKind::Tracklet));
    EXPECT_FALSE(framer.MakeTopic("cam 01", MessageKind::Tracklet));
    EXPECT_FALSE(framer.MakeTopic("cam-01", MessageKind::Unknown));
}

TEST(MessageFramer, RejectsEmptyAndOversizedPayloads) {
    FramerOptions options;
    options.max_payload_bytes = 8;
    const MessageFramer framer(options);

    EXPECT_FALSE(framer.ValidatePayload(""));
    EXPECT_TRUE(framer.ValidatePayload("12345678"));
    EXPECT_FALSE(framer.ValidatePayload("123456789"));
}

// ---------------------------------------------------------------------------
// SequenceTracker
// ---------------------------------------------------------------------------

TEST(SequenceTracker, DetectsGapsDuplicatesAndReordering) {
    SequenceTracker tracker;

    auto first = tracker.Observe("s", "cam", 10);
    EXPECT_EQ(first.kind, SequenceEventKind::First);

    auto inOrder = tracker.Observe("s", "cam", 11);
    EXPECT_EQ(inOrder.kind, SequenceEventKind::InOrder);

    auto gap = tracker.Observe("s", "cam", 15);
    EXPECT_EQ(gap.kind, SequenceEventKind::Gap);
    EXPECT_EQ(gap.missing, 3u);

    auto duplicate = tracker.Observe("s", "cam", 15);
    EXPECT_EQ(duplicate.kind, SequenceEventKind::Duplicate);

    auto reordered = tracker.Observe("s", "cam", 12);
    EXPECT_EQ(reordered.kind, SequenceEventKind::OutOfOrder);

    EXPECT_EQ(tracker.stats().observed, 5u);
    EXPECT_EQ(tracker.stats().gaps, 1u);
    EXPECT_EQ(tracker.stats().missing_total, 3u);
    EXPECT_EQ(tracker.stats().duplicates, 1u);
    EXPECT_EQ(tracker.stats().out_of_order, 1u);
}

TEST(SequenceTracker, SeparatesSessionsAndCameras) {
    SequenceTracker tracker;

    EXPECT_EQ(tracker.Observe("s1", "cam", 0).kind, SequenceEventKind::First);
    EXPECT_EQ(tracker.Observe("s2", "cam", 100).kind, SequenceEventKind::First);
    EXPECT_EQ(tracker.Observe("s1", "cam2", 50).kind, SequenceEventKind::First);
    EXPECT_EQ(tracker.Observe("s1", "cam", 1).kind, SequenceEventKind::InOrder);
    EXPECT_EQ(tracker.stats().streams, 3u);

    tracker.Reset();
    EXPECT_EQ(tracker.stats().observed, 0u);
    EXPECT_EQ(tracker.Observe("s1", "cam", 5).kind, SequenceEventKind::First);
}

// ---------------------------------------------------------------------------
// Global track codec
// ---------------------------------------------------------------------------

TEST(GlobalTrackCodec, RoundTripsBinaryAndJson) {
    GlobalTrackUpdate update;
    update.global_id = 42;
    update.type = tmcmt::ObjectType::Bus;
    update.is_final = true;
    update.updated_at = Timestamp(12'345);
    update.segments.push_back(MakeTracklet("s", "cam-1", 0));
    update.segments.push_back(MakeTracklet("s", "cam-2", 1));

    auto binary = tmcmt::GlobalTrackCodec::EncodeBinary(update);
    ASSERT_TRUE(binary);
    auto decoded = tmcmt::GlobalTrackCodec::DecodeBinary(binary.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().global_id, 42u);
    EXPECT_EQ(decoded.value().type, tmcmt::ObjectType::Bus);
    EXPECT_TRUE(decoded.value().is_final);
    EXPECT_EQ(decoded.value().updated_at, Timestamp(12'345));
    ASSERT_EQ(decoded.value().segments.size(), 2u);
    EXPECT_EQ(decoded.value().segments[0].camera, "cam-1");
    EXPECT_EQ(decoded.value().segments[1].camera, "cam-2");

    auto json = tmcmt::GlobalTrackCodec::EncodeJson(update, true);
    ASSERT_TRUE(json);
    auto fromJson = tmcmt::GlobalTrackCodec::DecodeJson(json.value());
    ASSERT_TRUE(fromJson);
    EXPECT_EQ(fromJson.value().global_id, 42u);
    ASSERT_EQ(fromJson.value().segments.size(), 2u);
}

TEST(GlobalTrackCodec, RejectsZeroGlobalIdAndForeignSchemaVersion) {
    GlobalTrackUpdate update;
    update.global_id = 0;
    auto binary = tmcmt::GlobalTrackCodec::EncodeBinary(update);
    ASSERT_TRUE(binary);
    auto decoded = tmcmt::GlobalTrackCodec::DecodeBinary(binary.value());
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.status().code(), ErrorCode::InvalidArgument);

    // Hand-craft a v2 message by patching the first varint field (field 1,
    // wire type 0 is schema_version).
    std::string payload = binary.value();
    // schema_version is encoded as tag 0x08 followed by the value. Find it and
    // replace the value with 2.
    const std::size_t tag = payload.find(static_cast<char>(0x08));
    ASSERT_NE(tag, std::string::npos);
    payload[tag + 1] = static_cast<char>(2);
    auto rejected = tmcmt::GlobalTrackCodec::DecodeBinary(payload);
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status().code(), ErrorCode::Unsupported);
}

TEST(GlobalIdMergeCodec, RoundTripsBinaryAndJson) {
    GlobalIdMergeEvent event;
    event.old_id = 7;
    event.new_id = 3;
    event.reason = "transition";
    event.ts = Timestamp(999);

    auto binary = tmcmt::GlobalIdMergeCodec::EncodeBinary(event);
    ASSERT_TRUE(binary);
    auto decoded = tmcmt::GlobalIdMergeCodec::DecodeBinary(binary.value());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().old_id, 7u);
    EXPECT_EQ(decoded.value().new_id, 3u);
    EXPECT_EQ(decoded.value().reason, "transition");
    EXPECT_EQ(decoded.value().ts, Timestamp(999));

    auto json = tmcmt::GlobalIdMergeCodec::EncodeJson(event);
    ASSERT_TRUE(json);
    auto fromJson = tmcmt::GlobalIdMergeCodec::DecodeJson(json.value());
    ASSERT_TRUE(fromJson);
    EXPECT_EQ(fromJson.value().old_id, 7u);
}

// ---------------------------------------------------------------------------
// Publisher / source
// ---------------------------------------------------------------------------

TEST(ZmqTransport, PublisherSourceRoundTrip) {
    ZmqTrackletSource source;
    ASSERT_TRUE(source.Start(SourceOptions("tcp://127.0.0.1:*")));

    ZmqTrackletPublisher publisher;
    ASSERT_TRUE(publisher.Start(PublisherOptions(source.endpoint())));

    ASSERT_TRUE(WaitForSubscription(source, publisher));
    DrainUntilQuiet(source);
    ASSERT_TRUE(source.Reset().ok());

    auto sent = MakeTracklet("roundtrip", "cam-rt", 0, 7);
    sent.quality = 0.75f;
    ASSERT_TRUE(publisher.Publish(sent));

    auto received = ReadSession(source, "roundtrip");
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->camera, "cam-rt");
    EXPECT_EQ(received->local_track_id, 7u);
    EXPECT_EQ(received->sequence, 0u);
    EXPECT_EQ(received->embedding.dim, 4u);
    EXPECT_FLOAT_EQ(received->quality, 0.75f);

    EXPECT_GE(source.stats().decoded, 1u);
    EXPECT_EQ(source.stats().missing_total, 0u);

    publisher.Stop();
    source.Stop();
}

TEST(ZmqTransport, SourceTimesOutWhenIdle) {
    ZmqTrackletSource source;
    auto options = SourceOptions("tcp://127.0.0.1:*");
    options.timeout_ms = 30;
    ASSERT_TRUE(source.Start(options));

    Tracklet tracklet;
    const auto begin = std::chrono::steady_clock::now();
    auto next = source.Next(tracklet);
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    ASSERT_FALSE(next);
    EXPECT_EQ(next.status().code(), ErrorCode::Timeout);
    EXPECT_GE(elapsed, 20ms);
    EXPECT_EQ(source.stats().timeouts, 1u);

    source.Stop();
}

TEST(ZmqTransport, StopUnblocksNext) {
    ZmqTrackletSource source;
    auto options = SourceOptions("tcp://127.0.0.1:*");
    options.timeout_ms = -1;
    ASSERT_TRUE(source.Start(options));

    auto future = std::async(std::launch::async, [&source] {
        Tracklet tracklet;
        return source.Next(tracklet);
    });

    std::this_thread::sleep_for(100ms);
    source.Stop();

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    auto result = future.get();
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value());
}

TEST(ZmqTransport, RejectsForeignSchemaVersion) {
    ZmqTrackletSource source;
    auto options = SourceOptions("tcp://127.0.0.1:*");
    options.strict = true;
    ASSERT_TRUE(source.Start(options));

    ZmqTrackletPublisher publisher;
    ASSERT_TRUE(publisher.Start(PublisherOptions(source.endpoint())));

    ASSERT_TRUE(WaitForSubscription(source, publisher));
    DrainUntilQuiet(source);
    ASSERT_TRUE(source.Reset().ok());

    auto bad = MakeTracklet("bad-version", "cam-bad", 0);
    bad.schema_version = 2;
    ASSERT_TRUE(publisher.Publish(bad));

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::optional<tmcmt::Status> failure;
    Tracklet tracklet;
    while (std::chrono::steady_clock::now() < deadline) {
        auto next = source.Next(tracklet);
        if (!next) {
            if (next.status().code() == ErrorCode::Timeout) {
                continue;
            }
            failure = next.status();
            break;
        }
        if (!next.value()) {
            break;
        }
    }

    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->code(), ErrorCode::Unsupported);

    publisher.Stop();
    source.Stop();
}

TEST(ZmqTransport, CountsSequenceGaps) {
    ZmqTrackletSource source;
    ASSERT_TRUE(source.Start(SourceOptions("tcp://127.0.0.1:*")));

    ZmqTrackletPublisher publisher;
    ASSERT_TRUE(publisher.Start(PublisherOptions(source.endpoint())));

    ASSERT_TRUE(WaitForSubscription(source, publisher));
    DrainUntilQuiet(source);
    ASSERT_TRUE(source.Reset().ok());

    ASSERT_TRUE(publisher.Publish(MakeTracklet("gap", "cam-gap", 0, 0)));
    ASSERT_TRUE(publisher.Publish(MakeTracklet("gap", "cam-gap", 1, 0)));
    ASSERT_TRUE(publisher.Publish(MakeTracklet("gap", "cam-gap", 3, 0)));

    EXPECT_EQ(ReadSessionCount(source, "gap", 3), 3u);
    EXPECT_EQ(source.stats().missing_total, 1u);

    publisher.Stop();
    source.Stop();
}

TEST(ZmqTransport, HandlesTenThousandMessages) {
    ZmqTrackletSource source;
    auto sourceOptions = SourceOptions("tcp://127.0.0.1:*");
    sourceOptions.timeout_ms = 100;
    ASSERT_TRUE(source.Start(sourceOptions));

    ZmqTrackletPublisher publisher;
    ASSERT_TRUE(publisher.Start(PublisherOptions(source.endpoint())));

    ASSERT_TRUE(WaitForSubscription(source, publisher));
    DrainUntilQuiet(source);
    ASSERT_TRUE(source.Reset().ok());

    constexpr std::size_t kMessages = 10'000;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kMessages; ++i) {
        ASSERT_TRUE(publisher.Publish(
            MakeTracklet("load", "cam-load", i, static_cast<std::uint64_t>(i / 5))));
    }
    const auto published = std::chrono::steady_clock::now();

    const std::size_t received = ReadSessionCount(source, "load", kMessages);
    const auto end = std::chrono::steady_clock::now();

    EXPECT_EQ(received, kMessages);
    EXPECT_EQ(source.stats().missing_total, 0u);

    const auto publishMs = std::chrono::duration_cast<std::chrono::milliseconds>(published - begin).count();
    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    EXPECT_LT(totalMs, 30'000);
    std::cout << "[          ] 10k messages: publish=" << publishMs << " ms, end-to-end=" << totalMs
              << " ms\n";

    publisher.Stop();
    source.Stop();
}

// ---------------------------------------------------------------------------
// Broker
// ---------------------------------------------------------------------------

TEST(ZmqBroker, ForwardsAndRecoversAfterRestart) {
    ZmqBrokerOptions brokerOptions;
    brokerOptions.frontend_endpoint = "tcp://127.0.0.1:*";
    brokerOptions.backend_endpoint = "tcp://127.0.0.1:*";

    ZmqBroker broker;
    ASSERT_TRUE(broker.Start(brokerOptions));
    const std::string frontend = broker.frontend_endpoint();
    const std::string backend = broker.backend_endpoint();

    ZmqTrackletSource source;
    auto sourceOptions = SourceOptions(backend);
    sourceOptions.bind = false;
    ASSERT_TRUE(source.Start(sourceOptions));

    ZmqTrackletPublisher publisher;
    ASSERT_TRUE(publisher.Start(PublisherOptions(frontend)));

    ASSERT_TRUE(WaitForSubscription(source, publisher, 10s));
    DrainUntilQuiet(source);
    ASSERT_TRUE(source.Reset().ok());

    ASSERT_TRUE(publisher.Publish(MakeTracklet("before", "cam-b", 0, 0)));
    EXPECT_TRUE(ReadSession(source, "before", 5s).has_value());

    broker.Stop();
    std::this_thread::sleep_for(200ms);

    brokerOptions.frontend_endpoint = frontend;
    brokerOptions.backend_endpoint = backend;
    ASSERT_TRUE(broker.Start(brokerOptions));

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool recovered = false;
    std::uint64_t sequence = 0;
    while (!recovered && std::chrono::steady_clock::now() < deadline) {
        (void)publisher.Publish(MakeTracklet("after", "cam-b", sequence, 0));
        ++sequence;
        recovered = ReadSession(source, "after", 200ms).has_value();
    }

    EXPECT_TRUE(recovered);
    EXPECT_GT(broker.stats().forwarded_to_subscribers, 0u);

    publisher.Stop();
    source.Stop();
    broker.Stop();
}

// ---------------------------------------------------------------------------
// Result sink
// ---------------------------------------------------------------------------

TEST(ZmqResultSink, PublishesGlobalUpdatesAndMergeEvents) {
    ZmqResultSink sink;
    ZmqResultSinkOptions sinkOptions;
    sinkOptions.endpoint = "tcp://127.0.0.1:*";
    sinkOptions.bind = true;
    ASSERT_TRUE(sink.Start(sinkOptions));

    RawSubscriber subscriber(sink.endpoint(), "tmcmt.v1.global.");

    GlobalTrackUpdate update;
    update.global_id = 11;
    update.type = tmcmt::ObjectType::Car;
    update.is_final = false;
    update.updated_at = Timestamp(555);
    update.segments.push_back(MakeTracklet("s", "cam-a", 0));

    std::string topic;
    std::string payload;
    bool gotUpdate = false;
    for (int attempt = 0; attempt < 100 && !gotUpdate; ++attempt) {
        ASSERT_TRUE(sink.Publish(update));
        gotUpdate = subscriber.Receive(topic, payload, 50);
    }
    ASSERT_TRUE(gotUpdate);
    EXPECT_EQ(topic, "tmcmt.v1.global.track");
    auto decodedUpdate = tmcmt::GlobalTrackCodec::DecodeBinary(payload);
    ASSERT_TRUE(decodedUpdate);
    EXPECT_EQ(decodedUpdate.value().global_id, 11u);
    ASSERT_EQ(decodedUpdate.value().segments.size(), 1u);

    GlobalIdMergeEvent event;
    event.old_id = 20;
    event.new_id = 11;
    event.reason = "merge";
    event.ts = Timestamp(666);

    bool gotMerge = false;
    for (int attempt = 0; attempt < 100 && !gotMerge; ++attempt) {
        ASSERT_TRUE(sink.PublishMerge(event));
        if (subscriber.Receive(topic, payload, 50) && topic == "tmcmt.v1.global.events") {
            gotMerge = true;
        }
    }
    ASSERT_TRUE(gotMerge);
    auto decodedMerge = tmcmt::GlobalIdMergeCodec::DecodeBinary(payload);
    ASSERT_TRUE(decodedMerge);
    EXPECT_EQ(decodedMerge.value().old_id, 20u);
    EXPECT_EQ(decodedMerge.value().new_id, 11u);

    sink.Stop();
}
