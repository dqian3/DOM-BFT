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

    std::vector<std::string> clientIps;
    int clientPort;
    std::string clientKeysDir;
    int clientRuntimeSeconds;
    int clientNormalPathTimeout;
    int clientSlowPathTimeout;
    int clientMaxInFlight;
    int clientSendRate;
    std::string clientSendMode;

    std::vector<std::string> proxyIps;
    int proxyForwardPort;
    int proxyMeasurementPort;
    int proxyShards;
    std::string proxyKeysDir;
    uint32_t proxyMaxOwd;

    std::vector<std::string> receiverIps;
    int receiverPort;
    std::string receiverKeysDir;
    int receiverShards;
    bool receiverLocal;
    int numVerifyThreads;

    std::vector<std::string> replicaIps;
    int replicaPort;
    int replicaFallbackStartTimeout;
    int replicaFallbackTimeout;
    std::string replicaKeysDir;
    int replicaNumSendThreads;
    int replicaNumVerifyThreads;

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
            clientKeysDir = parseField<std::string>(clientNode, "keysDir");
            clientRuntimeSeconds = parseField<int>(clientNode, "runtimeSeconds");
            clientNormalPathTimeout = parseField<int>(clientNode, "normalPathTimeout");
            clientSlowPathTimeout = parseField<int>(clientNode, "slowPathTimeout");
            clientMaxInFlight = parseField<int>(clientNode, "maxInFlight");
            clientSendRate = parseField<int>(clientNode, "sendRate");
            clientSendMode = parseField<std::string>(clientNode, "sendMode");
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
            proxyMeasurementPort = parseField<int>(proxyNode, "measurementPort");
            proxyKeysDir = parseField<std::string>(proxyNode, "keysDir");
            proxyMaxOwd = parseField<int>(proxyNode, "maxOwd");
        } catch (const ConfigParseException &e) {
            throw ConfigParseException("Error parsing proxy " + std::string(e.what()));
        }
    }

    void parseReceiverConfig(const YAML::Node &root)
    {
        const YAML::Node &receiverNode = root["receiver"];
        std::string key;

        try {
            parseStringVector(receiverIps, receiverNode, "ips");
            receiverPort = parseField<int>(receiverNode, "port");
            receiverKeysDir = parseField<std::string>(receiverNode, "keysDir");
            receiverShards = parseField<int>(receiverNode, "shards");
            receiverLocal = parseField<bool>(receiverNode, "local");
            numVerifyThreads = parseField<int>(receiverNode, "numVerifyThreads");
        } catch (const ConfigParseException &e) {
            throw ConfigParseException("Error parsing receiver " + std::string(e.what()));
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

            replicaFallbackStartTimeout = parseField<int>(replicaNode, "fallbackStartTimeout");
            replicaFallbackTimeout = parseField<int>(replicaNode, "fallbackTimeout");

            replicaNumVerifyThreads = parseField<int>(replicaNode, "numVerifyThreads");
            replicaNumSendThreads = parseField<int>(replicaNode, "numSendThreads");

        } catch (const ConfigParseException &e) {
            throw ConfigParseException("Error parsing replica " + std::string(e.what()));
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
        } else {
            throw ConfigParseException("Invalid app type");
        }

        parseClientConfig(config);
        parseProxyConfig(config);
        parseReceiverConfig(config);
        parseReplicaConfig(config);

        // TODO do some verification
        // number of receivers = number of replicas
        // number of replcias > 3f + 1?
        // etc.
    }
};

#endif