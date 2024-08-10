#ifndef PROCESS_CONFIG_H
#define PROCESS_CONFIG_H

#include <stdint.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

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

    std::vector<std::string> clientIps;
    int clientPort;
    std::string clientKeysDir;
    int clientMaxRequests;
    int clientNumRequests;
    int clientRuntimeSeconds;
    int clientNormalPathTimeout;
    int clientSlowPathTimeout;

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

    std::vector<std::string> replicaIps;
    int replicaPort;
    std::string replicaKeysDir;

    template <class T> T parseField(const YAML::Node &parent, const std::string &key)
    {
        if (!parent[key]) {
            throw ConfigParseException("'" + key + "' not found");
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
            clientMaxRequests = parseField<int>(clientNode, "maxRequests");
            clientNumRequests = parseField<int>(clientNode, "numRequests");
            clientRuntimeSeconds = parseField<int>(clientNode, "runtimeSeconds");
            clientNormalPathTimeout = parseField<int>(clientNode, "normalPathTimeout");
            clientSlowPathTimeout = parseField<int>(clientNode, "slowPathTimeout");

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