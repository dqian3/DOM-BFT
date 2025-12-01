#ifndef PROCESS_CONFIG_H
#define PROCESS_CONFIG_H

#include <stdint.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "lib/application.h"

class ConfigParseException : public std::runtime_error {
public:
    ConfigParseException(const std::string &msg)
        : std::runtime_error(msg)
    {
    }

    static ConfigParseException missing(const std::string &field)
    {
        return ConfigParseException("Config missing field " + field);
    }
};

struct ProcessConfig {
    std::string transport;
    AppType app;
    std::string appStr;

    uint32_t f;
    uint32_t e;

    std::vector<std::string> clientIps;
    int clientPort;
    std::string clientKeysDir;
    int clientRuntimeSeconds;
    int clientNormalPathTimeout;
    int clientRequestTimeout;
    int clientMaxInFlight;
    int clientSendRate;
    std::string clientSendMode;
    int clientRequestSize;
    bool clientUseHMAC;
    bool clientSendProofs;
    bool clientNormalPathEnabled;

    // Temporary rate increase configuration (optional sub-object)
    struct {
        bool enabled;
        uint32_t seqThreshold;
        uint32_t durationUs;
        uint32_t increasedSendRate;
        uint32_t increasedMaxInFlight;
    } clientTemporaryRateIncrease;

    std::vector<std::string> proxyIps;
    int proxyForwardPort;
    int proxyMeasurementPort;
    int proxyShards;
    float proxyOffsetCoefficient;
    std::string proxyKeysDir;
    uint32_t proxyMaxOwd;
    bool proxyBatchEnabled;
    uint32_t proxyBatchMaxCount;
    uint32_t proxyBatchMaxDelay;

    std::vector<std::string> replicaIps;
    int replicaPort;
    int replicaCheckpointTimeout;
    int replicaRepairTimeout;
    int replicaRepairViewTimeout;

    std::string replicaKeysDir;
    int replicaNumSendThreads;
    int replicaNumVerifyThreads;
    uint32_t replicaCheckpointInterval;
    uint32_t replicaSnapshotInterval;

    // Unified mode configuration
    bool unifiedMode;

    // Preserialization mode configuration
    std::string preserializationMode;  // "disabled", "full", or "order"

    // Proxy configuration
    bool useProxy;
    bool sendToLeader;  // When useProxy=false, send only to leader (replica 0) instead of all replicas

    template <class T> T parseField(const YAML::Node &parent, const std::string &key)
    {
        if (!parent[key]) {
            throw ConfigParseException("'" + key + "' not found, required");
        }

        try {
            return parent[key].as<T>();
        } catch (const YAML::BadConversion &e) {
            throw ConfigParseException("'" + key + "': " + e.msg + ".");
        }
    }

    template <class T> T parseField(const YAML::Node &parent, const std::string &key, const T &default_value)
    {
        if (!parent[key]) {
            return default_value;
        }

        try {
            return parent[key].as<T>();
        } catch (const YAML::BadConversion &e) {
            throw ConfigParseException("'" + key + "': " + e.msg + ".");
        }
    }

    void parseStringVector(std::vector<std::string> &list, const YAML::Node &parent, const std::string &key)
    {
        if (!parent[key]) {
            throw ConfigParseException("'" + key + "' not found");
        }

        try {
            for (uint32_t i = 0; i < parent[key].size(); i++) {
                list.push_back(parent[key][i].as<std::string>());
            }
        } catch (const YAML::BadConversion &e) {
            throw ConfigParseException("'" + key + "': " + e.msg + ".");
        }
    }

    void parseClientConfig(const YAML::Node &root)
    {
        const YAML::Node &clientNode = root["client"];
        std::string key;

        try {
            parseStringVector(clientIps, clientNode, "ips");
            clientPort = parseField<int>(clientNode, "port");
            clientKeysDir = parseField<std::string>(clientNode, "keysDir", "keys/client");
            clientRuntimeSeconds = parseField<int>(clientNode, "runtimeSeconds");
            clientNormalPathTimeout = parseField<int>(clientNode, "normalPathTimeout");
            clientRequestTimeout = parseField<int>(clientNode, "requestTimeout");
            clientMaxInFlight = parseField<int>(clientNode, "maxInFlight");
            clientSendRate = parseField<int>(clientNode, "sendRate");
            clientSendMode = parseField<std::string>(clientNode, "sendMode");
            clientRequestSize = parseField<int>(clientNode, "requestSize");

            // TODO these are more global process config than client-specific
            clientUseHMAC = parseField<bool>(clientNode, "useHMAC", false);
            clientSendProofs = parseField<bool>(clientNode, "sendProofs", false);
            clientNormalPathEnabled = parseField<bool>(clientNode, "normalPathEnabled", false);

            // Parse temporary rate increase configuration (optional)
            if (clientNode["temporaryRateIncrease"]) {
                const YAML::Node &rateIncreaseNode = clientNode["temporaryRateIncrease"];
                clientTemporaryRateIncrease.enabled = parseField<bool>(rateIncreaseNode, "enabled", false);
                clientTemporaryRateIncrease.seqThreshold = parseField<uint32_t>(rateIncreaseNode, "seqThreshold", 0);
                clientTemporaryRateIncrease.durationUs = parseField<uint32_t>(rateIncreaseNode, "durationUs", 10000000); // Default 10s
                clientTemporaryRateIncrease.increasedSendRate = parseField<uint32_t>(rateIncreaseNode, "increasedSendRate", 0);
                clientTemporaryRateIncrease.increasedMaxInFlight = parseField<uint32_t>(rateIncreaseNode, "increasedMaxInFlight", 0);
            } else {
                // Default: disabled
                clientTemporaryRateIncrease.enabled = false;
                clientTemporaryRateIncrease.seqThreshold = 0;
                clientTemporaryRateIncrease.durationUs = 10000000;
                clientTemporaryRateIncrease.increasedSendRate = 0;
                clientTemporaryRateIncrease.increasedMaxInFlight = 0;
            }
        }

        catch (const ConfigParseException &e) {
            throw ConfigParseException("Error parsing client " + std::string(e.what()));
        }
    }

    void parseProxyConfig(const YAML::Node &root)
    {
        const YAML::Node &proxyNode = root["proxy"];
        std::string key;

        try {
            parseStringVector(proxyIps, proxyNode, "ips");
            proxyShards = parseField<int>(proxyNode, "shards");
            proxyForwardPort = parseField<int>(proxyNode, "forwardPort");
            proxyKeysDir = parseField<std::string>(proxyNode, "keysDir");
            proxyMaxOwd = parseField<int>(proxyNode, "maxOwd");
            proxyOffsetCoefficient = parseField<float>(proxyNode, "offsetCoefficient", 1.5);
            proxyBatchEnabled = parseField<bool>(proxyNode, "proxyBatchEnabled", false);
            proxyBatchMaxCount = parseField<uint32_t>(proxyNode, "proxyBatchMaxCount", 50);
            proxyBatchMaxDelay = parseField<uint32_t>(proxyNode, "proxyBatchMaxDelay", 5000);

        } catch (const ConfigParseException &e) {
            throw ConfigParseException("Error parsing proxy " + std::string(e.what()));
        }
    }

    void parseReplicaConfig(const YAML::Node &root)
    {
        const YAML::Node &replicaNode = root["replica"];
        std::string key;

        try {
            parseStringVector(replicaIps, replicaNode, "ips");
            replicaPort = parseField<int>(replicaNode, "port");
            replicaKeysDir = parseField<std::string>(replicaNode, "keysDir");

            replicaRepairTimeout = parseField<int>(replicaNode, "repairTimeout");
            replicaCheckpointTimeout = parseField<int>(replicaNode, "checkpointTimeout");
            replicaRepairViewTimeout = parseField<int>(replicaNode, "repairViewTimeout");

            replicaNumVerifyThreads = parseField<int>(replicaNode, "numVerifyThreads");
            replicaNumSendThreads = parseField<int>(replicaNode, "numSendThreads");

            replicaCheckpointInterval = parseField<int>(replicaNode, "checkpointInterval");
            replicaSnapshotInterval = parseField<int>(replicaNode, "snapshotInterval", replicaCheckpointInterval);

            if (replicaSnapshotInterval % replicaCheckpointInterval != 0) {
                throw ConfigParseException("Snapshot interval must be a multiple of checkpoint interval");
            }

            unifiedMode = parseField<bool>(replicaNode, "unifiedMode", false);

        } catch (const ConfigParseException &e) {
            throw ConfigParseException("Error parsing replica config: " + std::string(e.what()));
        }
    }

    void parseConfig(const std::string &configFilename)
    {
        YAML::Node config;

        try {
            config = YAML::LoadFile(configFilename);
        } catch (const YAML::BadFile &e) {
            throw ConfigParseException("Error loading config file:" + e.msg + ".");
        }

        transport = parseField<std::string>(config, "transport");
        app = parseField<std::string>(config, "app") == "counter" ? AppType::COUNTER : AppType::KV_STORE;
        appStr = parseField<std::string>(config, "app");
        if (appStr == "counter") {
            app = AppType::COUNTER;
        } else if (appStr == "kv_store") {
            app = AppType::KV_STORE;
        } else {
            throw ConfigParseException("Invalid app type " + appStr + ". Must be 'counter' or 'kv_store'");
        }

        auto resiliencyParams =
            parseField<std::unordered_map<std::string, u_int32_t>>(config, "resiliency", {{"f", 1}, {"e", 1}});
        f = resiliencyParams.at("f");
        e = resiliencyParams.at("e");

        // Parse top-level preserialization option
        preserializationMode = parseField<std::string>(config, "preserializationMode", "disabled");
        if (preserializationMode != "disabled" && preserializationMode != "full" && preserializationMode != "order") {
            throw ConfigParseException("Invalid preserializationMode '" + preserializationMode + "'. Must be 'disabled', 'full', or 'order'");
        }

        // Parse top-level proxy option
        useProxy = parseField<bool>(config, "useProxy", true);
        sendToLeader = parseField<bool>(config, "sendToLeader", false);

        // Validate: if preserialization is enabled, useProxy must be false
        if (preserializationMode != "disabled" && useProxy) {
            throw ConfigParseException("When preserializationMode is enabled ('" + preserializationMode + "'), useProxy must be false");
        }

        parseClientConfig(config);
        parseProxyConfig(config);
        parseReplicaConfig(config);

        // TODO do some verification
        // number of replicas > 3f + 1?
        // etc.
    }
};

#endif