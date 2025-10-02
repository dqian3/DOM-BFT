#include "lib/crypto/hmac_provider.h"
#include "lib/crypto/sig_provider.h"
#include "lib/transport/endpoint.h"
#include "lib/transport/nng_endpoint_threaded.h"
#include "lib/transport/ooo_rpc_endpoint.h"
#include "lib/transport/udp_endpoint.h"

#include <glog/logging.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

static std::atomic<uint64_t> recv_count{0};
static std::atomic<int64_t> total_latency_us{0};

// Sequence number tracking for UDP drop detection (single sender)
static std::atomic<uint64_t> total_sent{0};
static std::atomic<uint64_t> expected_seq{1};
static std::atomic<uint64_t> total_drops{0};

// Latency histogram buckets (in microseconds)
static std::atomic<uint64_t> latency_buckets[10] = {
    0
};   // 0-100us, 100-500us, 500us-1ms, 1-5ms, 5-10ms, 10-20ms, 20-50ms, 50-100ms, 100-500ms, 500ms+
static std::atomic<uint64_t> min_latency_us{UINT64_MAX};
static std::atomic<uint64_t> max_latency_us{0};

void updateLatencyStats(uint64_t latency_us)
{
    // Update min/max
    uint64_t current_min = min_latency_us.load();
    while (latency_us < current_min && !min_latency_us.compare_exchange_weak(current_min, latency_us)) {
    }

    uint64_t current_max = max_latency_us.load();
    while (latency_us > current_max && !max_latency_us.compare_exchange_weak(current_max, latency_us)) {
    }

    // Update histogram
    int bucket = 9;   // default to 500ms+ bucket
    if (latency_us < 100)
        bucket = 0;   // 0-100us
    else if (latency_us < 500)
        bucket = 1;   // 100-500us
    else if (latency_us < 1000)
        bucket = 2;   // 500us-1ms
    else if (latency_us < 5000)
        bucket = 3;   // 1-5ms
    else if (latency_us < 10000)
        bucket = 4;   // 5-10ms
    else if (latency_us < 20000)
        bucket = 5;   // 10-20ms
    else if (latency_us < 50000)
        bucket = 6;   // 20-50ms
    else if (latency_us < 100000)
        bucket = 7;   // 50-100ms
    else if (latency_us < 500000)
        bucket = 8;   // 100-500ms

    latency_buckets[bucket].fetch_add(1);
}

int main(int argc, char *argv[])
{
    if (argc < 10) {
        LOG(INFO) << "Usage: " << argv[0]
                  << " <listen_port> <peer_address> <peer_port> <message_size> <endpoint_type> <crypto type> "
                     "<send_interval_us> <num_verify_threads> <num_senders>\n";
        return 1;
    }

    int listen_port = std::stoi(argv[1]);
    std::string peer_address = argv[2];
    int peer_port = std::stoi(argv[3]);
    int message_size = std::stoi(argv[4]);
    std::string endpoint_type = argv[5];
    std::string crypto_type = argv[6];
    int send_interval_us = std::stoi(argv[7]);

    int num_verify_threads = std::stoi(argv[8]);
    int num_senders = 1;

    if (endpoint_type == "ooo") {
        num_senders = std::stoi(argv[9]);
    } else {
        LOG(INFO) << "num_senders argument is ignored for endpoint type " << endpoint_type << "\n";
    }

    // ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    Address peer_addr(peer_address, peer_port);

    // ---- choose endpoint implementation ----
    Endpoint *endpoint = nullptr;
    if (endpoint_type == "ooo") {
        endpoint = new OOORPCEndpoint("0.0.0.0", listen_port, {peer_addr}, num_senders);
    } else if (endpoint_type == "nng") {
        endpoint = new NngEndpointThreaded({{Address("0.0.0.0", listen_port), peer_addr}});
    } else if (endpoint_type == "udp") {
        endpoint = new UDPEndpoint("0.0.0.0", listen_port);
    } else {
        LOG(INFO) << "Unknown endpoint type: " << endpoint_type << "\n";
        return 1;
    }

    SignatureProvider sigProvider;
    HMACProvider hmacProvider;

    if (crypto_type == "sig") {
        LOG(INFO) << "Using Signatures";
        sigProvider.loadPrivateKey("keys/client/client0.der");
        sigProvider.loadPublicKeys(NodeType::CLIENT, "keys/client");

    } else if (crypto_type == "hmac") {
        LOG(ERROR) << "Using HMAC";
        hmacProvider.loadClientKeysDev({NodeType::CLIENT, 0}, 1);
    } else {
        LOG(INFO) << "No crypto specificied";
    }

    // ---- receiver side stats ----
    static uint64_t first_msg_time;
    static uint64_t end_time;
    static std::atomic<bool> started{false};

    std::vector<std::thread> verifyThreads_;
    BlockingConcurrentQueue<std::vector<byte>> verifyQueue_;

    for (int i = 0; i < num_verify_threads; ++i) {
        verifyThreads_.emplace_back([&] {
            while (true) {
                std::vector<byte> msg;
                if (!verifyQueue_.wait_dequeue_timed(msg, 50000)) {
                    continue;
                }

                if (msg.empty()) {
                    return;
                }

                MessageHeader *hdr = (MessageHeader *) msg.data();

                if (crypto_type == "sig") {
                    if (!sigProvider.verify(hdr, {NodeType::CLIENT, 0})) {
                        LOG(INFO) << "Failed to verify signature";
                        continue;
                    }
                } else if (crypto_type == "hmac") {
                    if (!hmacProvider.verify(hdr, {NodeType::CLIENT, 0})) {
                        LOG(INFO) << "Failed to verify HMAC";
                        continue;
                    }
                }
                recv_count++;
            }
        });
    }

    endpoint->RegisterMsgHandler([&](MessageHeader *msgHdr, byte *msgBuffer, Address *sender) {
        if (!started.exchange(true)) {
            first_msg_time = GetMicrosecondTimestamp();

            started.notify_all();
            LOG(INFO) << "First message received, starting 10-second window\n";
        } else if (msgHdr->msgLen >= 2 * sizeof(uint64_t)) {
            // Extract timestamp and sequence number from message header
            uint64_t send_time_us = *reinterpret_cast<uint64_t *>(msgBuffer);
            uint64_t recv_seq = *reinterpret_cast<uint64_t *>(msgBuffer + 8);

            // Check for drops (UDP only)
            if (endpoint_type == "udp") {
                uint64_t expected = expected_seq.load();
                if (recv_seq > expected) {
                    uint64_t dropped = recv_seq - expected;
                    total_drops.fetch_add(dropped);
                    VLOG(2) << "Detected " << dropped << " dropped packets. Expected: " << expected
                            << ", Received: " << recv_seq;
                }
                expected_seq.store(recv_seq + 1);
            }

            // Calculate latency
            uint64_t recv_time_us = GetMicrosecondTimestamp();
            uint64_t latency_us = recv_time_us - send_time_us;

            if (recv_time_us < send_time_us)
                LOG(WARNING) << "Clock skew detected, recv time " << recv_time_us << " < send time " << send_time_us;

            assert(recv_time_us >= send_time_us);

            total_latency_us.fetch_add(latency_us);
            updateLatencyStats(latency_us);

            // Log individual latency for analysis
            LOG(INFO) << "LATENCY_SAMPLE seq=" << recv_seq << " latency_us=" << latency_us
                      << " recv_time=" << recv_time_us;
        }

        // Calculate latency from embedded timestamp
        if (msgHdr->msgLen >= sizeof(uint64_t)) {
            uint64_t send_time_us = *reinterpret_cast<uint64_t *>(msgBuffer);
            auto now = std::chrono::steady_clock::now();
            uint64_t recv_time_us =
                std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            uint64_t latency_us = recv_time_us - send_time_us;
            total_latency_us.fetch_add(latency_us);
        }

        if (crypto_type != "hmac" && crypto_type != "sig") {
            recv_count++;
            return;
        } else {
            byte *rawMsg = (byte *) msgHdr;
            verifyQueue_.enqueue(
                std::vector<byte>(rawMsg, rawMsg + sizeof(MessageHeader) + msgHdr->msgLen + msgHdr->sigLen)
            );
        }
    });

    endpoint->Connect();

    // ---- Timer to stop after 10 seconds from first message ----
    Timer stop_timer(
        [&](void * /*data*/, void *ep_void) {
            end_time = GetMicrosecondTimestamp();

            if (verifyQueue_.size_approx() == 0 && started && end_time - first_msg_time >= 10'000'000) {
                static_cast<Endpoint *>(ep_void)->LoopBreak();
            }
        },
        1'000   // check every 1 seconds
    );
    endpoint->RegisterTimer(&stop_timer);

    // ---- start receiver loop in background ----
    std::thread loop_thr([&] { endpoint->LoopRun(); });

    // small delay to ensure receiver is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::vector<std::thread> senders;

    for (int i = 0; i < num_senders; ++i) {
        senders.emplace_back([&, i] {
            char buf[message_size + 1024];

            // Create message with timestamp and sequence number at the beginning
            std::vector<byte> msg_with_header(message_size);
            uint64_t seq_num = 1;

            uint64_t now = GetMicrosecondTimestamp();
            *reinterpret_cast<uint64_t *>(msg_with_header.data()) = now;           // timestamp
            *reinterpret_cast<uint64_t *>(msg_with_header.data() + 8) = seq_num;   // sequence number
            // Fill the rest with 'a'
            std::fill(msg_with_header.begin() + 2 * sizeof(uint64_t), msg_with_header.end(), 'a');

            LOG(INFO) << "[sender " << i << "] sending first message\n";

            auto *hdr =
                endpoint->PrepareMsg(reinterpret_cast<const byte *>(msg_with_header.data()), msg_with_header.size(), 2);

            total_sent.fetch_add(1);

            if (crypto_type == "sig") {
                bool ret = sigProvider.appendSignature(hdr, sizeof(buf));
            } else if (crypto_type == "hmac") {
                bool ret = hmacProvider.appendMAC(hdr, sizeof(buf), {NodeType::CLIENT, 0});
            }

            endpoint->SendPreparedMsgTo(peer_addr, hdr);

            // Wait until main thread sets started = true
            if (endpoint_type != "udp") {
                started.wait(false);
                LOG(INFO) << "[sender " << i << "] received first message, continuing\n";
            }

            while (true) {
                now = GetMicrosecondTimestamp();
                if (started && (now - first_msg_time) >= 10'000'000) {
                    LOG(INFO) << "[sender " << i << "] finished after 10 s\n";
                    return;
                }

                seq_num++;
                *reinterpret_cast<uint64_t *>(msg_with_header.data()) = now;           // timestamp
                *reinterpret_cast<uint64_t *>(msg_with_header.data() + 8) = seq_num;   // sequence number

                auto *hdr = endpoint->PrepareMsg(
                    msg_with_header.data(), msg_with_header.size(), DUMMY_PROTO, (byte *) buf, sizeof(buf)
                );

                total_sent.fetch_add(1);

                if (send_interval_us > 0) {
                    usleep(send_interval_us);
                }

                if (crypto_type == "sig") {
                    sigProvider.appendSignature(hdr, sizeof(buf));
                } else if (crypto_type == "hmac") {
                    hmacProvider.appendMAC(hdr, sizeof(buf), {NodeType::CLIENT, 0});
                }

                endpoint->SendPreparedMsgTo(peer_addr, hdr);
            }
        });
    }

    // join all
    for (auto &t : senders)
        t.join();

    loop_thr.join();

    for (int i = 0; i < num_verify_threads; ++i) {
        verifyQueue_.enqueue(std::vector<byte>{});
    }
    for (auto &t : verifyThreads_) {
        t.join();
    }

    double secs = (end_time - first_msg_time) / 1'000'000.0;
    uint64_t c = recv_count.load();
    uint64_t sent = total_sent.load();
    uint64_t drops = total_drops.load();

    LOG(INFO) << "Received " << c << " messages in " << secs << " s:  " << (c / secs) << " msgs/s";

    if (endpoint_type == "udp" && sent > 0) {
        double drop_rate = (double) drops / sent * 100.0;
        LOG(INFO) << "Packet drops: " << drops << " out of " << sent << " sent (" << drop_rate << "% drop rate)";
    }

    if (c > 0) {
        double avg_latency_us = (double) total_latency_us / c;
        LOG(INFO) << "Average one-way latency: " << (avg_latency_us / 1000.0) << " ms";

        uint64_t min_lat = min_latency_us.load();
        uint64_t max_lat = max_latency_us.load();
        if (min_lat != UINT64_MAX) {
            LOG(INFO) << "Min latency: " << (min_lat / 1000.0) << " ms, Max latency: " << (max_lat / 1000.0) << " ms";
        }

        // Print distribution
        const char *bucket_labels[10] = {"0-100us", "100-500us", "500us-1ms", "1-5ms",     "5-10ms",
                                         "10-20ms", "20-50ms",   "50-100ms",  "100-500ms", "500ms+"};

        LOG(INFO) << "Latency distribution:";
        for (int i = 0; i < 10; i++) {
            uint64_t count = latency_buckets[i].load();
            if (count > 0) {
                double percentage = (double) count / c * 100.0;
                LOG(INFO) << "  " << bucket_labels[i] << ": " << count << " (" << percentage << "%)";
            }
        }
    }

    return 0;
}
