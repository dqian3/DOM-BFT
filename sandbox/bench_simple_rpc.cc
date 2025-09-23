#include "lib/hmac_provider.h"
#include "lib/signature_provider.h"
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

int main(int argc, char *argv[])
{
    if (argc < 5) {
        LOG(INFO) << "Usage: " << argv[0]
                  << " <listen_port> <peer_address> <peer_port> <message_size> <endpoint_type> <send_interval_us> "
                     "<crypto type> [num_senders]\n";
        return 1;
    }

    int listen_port = std::stoi(argv[1]);
    std::string peer_address = argv[2];
    int peer_port = std::stoi(argv[3]);
    int message_size = std::stoi(argv[4]);
    std::string endpoint_type = argv[5];
    int send_interval_us = std::stoi(argv[6]);
    std::string crypto_type = argv[6];

    int num_senders = 1;

    if (endpoint_type == "ooo") {
        num_senders = std::stoi(argv[7]);
    }

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

    } else if (crypto_type == "hmac") {
        LOG(ERROR) << "Using HMAC";

    } else {
        LOG(INFO) << "No crypto specificied";
    }

    // ---- receiver side stats ----
    static std::atomic<uint64_t> recv_count{0};
    static std::chrono::steady_clock::time_point first_msg_time;
    static std::atomic<bool> started{false};

    endpoint->RegisterMsgHandler([](MessageHeader * /*msgHdr*/, byte * /*msgBuffer*/, Address * /*sender*/) {
        if (!started.exchange(true)) {
            first_msg_time = std::chrono::steady_clock::now();

            started.notify_all();
            LOG(INFO) << "First message received, starting 10-second window\n";
        }
        ++recv_count;
    });

    endpoint->Connect();

    // ---- Timer to stop after 10 seconds from first message ----
    Timer stop_timer(
        [](void * /*data*/, void *ep_void) {
            auto now = std::chrono::steady_clock::now();
            if (started && std::chrono::duration_cast<std::chrono::seconds>(now - first_msg_time).count() >= 10) {
                double secs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - first_msg_time).count() / 1000.0;
                uint64_t c = recv_count.load();
                LOG(INFO) << "Received " << c << " messages in " << secs << " s:  " << (c / secs) << " msgs/s\n";
                static_cast<Endpoint *>(ep_void)->LoopBreak();
                exit(0);
            }
        },
        1'000   // check after 10 seconds
    );
    endpoint->RegisterTimer(&stop_timer);

    // ---- start receiver loop in background ----
    std::thread loop_thr([&] { endpoint->LoopRun(); });

    // small delay to ensure receiver is ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<std::thread> senders;

    for (int i = 0; i < num_senders; ++i) {
        senders.emplace_back([&, i] {
            char buf[message_size + 1024];

            std::string msg(message_size, 'a');
            LOG(INFO) << "[sender " << i << "] sending first message\n";

            auto *hdr = endpoint->PrepareMsg(reinterpret_cast<const byte *>(msg.data()), msg.size(), 2);
            endpoint->SendPreparedMsgTo(peer_addr, hdr);

            // Wait until main thread sets started = true

            if (endpoint_type != "udp") {
                started.wait(false);
                VLOG(1) << "[sender " << i << "] received first message, continuing\n";
            }

            while (true) {
                auto now = std::chrono::steady_clock::now();
                if (started && std::chrono::duration_cast<std::chrono::seconds>(now - first_msg_time).count() >= 10) {
                    LOG(INFO) << "[sender " << i << "] finished after 10 s\n";
                    return;
                }
                auto *hdr = endpoint->PrepareMsg(
                    reinterpret_cast<const byte *>(msg.data()), msg.size(), 2, (byte *) buf, sizeof(buf)
                );

                if (send_interval_us > 0) {
                    usleep(send_interval_us);
                }

                endpoint->SendPreparedMsgTo(peer_addr, hdr);
            }
        });
    }

    // join all
    for (auto &t : senders)
        t.join();

    loop_thr.join();

    return 0;
}
