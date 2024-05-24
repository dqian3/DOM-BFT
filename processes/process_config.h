#ifndef PROCESS_CONFIG_H
#define PROCESS_CONFIG_H

#include <glog/logging.h>
#include <stdint.h>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>

class ConfigParseException : public std::exception
{
public:
    std::string message;

    ConfigParseException(const std::string &msg) : message(msg) {}
    const char *what()
    {
        return message.c_str();
    }

    static ConfigParseException missing(const std::string &field)
    {
        return ConfigParseException("Config missing field " + field);
    }
};

struct ProcessConfig
{
    std::vector<std::string> clientIps;
    int clientPort;
    std::string clientKeysDir;
    int clientMaxRequests;

    std::vector<std::string> proxyIps;
    int proxyForwardPortBase;
    int proxyMeasurementPort;
    int proxyShards;
    std::string proxyKeysDir;
    uint32_t proxyInitialOwd;
    uint32_t proxyMaxOwd;

    std::vector<std::string> receiverIps;
    int receiverPort;
    std::string receiverKeysDir;
    int receiverShards;

    std::vector<std::string> replicaIps;
    int replicaPort;
    std::string replicaKeysDir;

    template <class T>
    T parseField(const YAML::Node &parent, const std::string &key)
    {
        if (!parent[key])
        {
            throw ConfigParseException("'" + key + "' not found");
        }

        try
        {
            return parent[key].as<T>();
        }
        catch (const YAML::BadConversion &e)
        {
            throw ConfigParseException("'" + key + "': " + e.msg + ".");
        }
    }

    void parseStringVector(std::vector<std::string> &list, const YAML::Node &parent, const std::string &key)
    {
        if (!parent[key])
        {
            throw ConfigParseException("'" + key + "' not found");
        }

        try
        {
            for (uint32_t i = 0; i < parent[key].size(); i++)
            {
                list.push_back(parent[key][i].as<std::string>());
            }
        }
        catch (const YAML::BadConversion &e)
        {
            throw ConfigParseException("'" + key + "': " + e.msg + ".");
        }
    }

    void parseClientConfig(const YAML::Node &root)
    {
        const YAML::Node &clientNode = root["client"];
        std::string key;

        try
        {
            parseStringVector(clientIps, clientNode, "ips");
            clientPort = parseField<int>(clientNode, "port");
            clientKeysDir = parseField<std::string>(clientNode, "keysDir");
            clientMaxRequests = parseField<int>(clientNode, "maxRequests");
        }
        catch (const ConfigParseException &e)
        {
            throw ConfigParseException("Error parsing client " + e.message);
        }
    }

    void parseProxyConfig(const YAML::Node &root)
    {
        const YAML::Node &proxyNode = root["proxy"];
        std::string key;

        try
        {
            parseStringVector(proxyIps, proxyNode, "ips");
            proxyShards = parseField<int>(proxyNode, "shards");
            proxyForwardPortBase = parseField<int>(proxyNode, "forwardPortBase");
            proxyMeasurementPort = parseField<int>(proxyNode, "measurementPort");
            proxyKeysDir = parseField<std::string>(proxyNode, "keysDir");
            proxyInitialOwd = parseField<int>(proxyNode, "initialOwd");
            proxyMaxOwd = parseField<int>(proxyNode, "maxOwd");
        }
        catch (const ConfigParseException &e)
        {
            throw ConfigParseException("Error parsing proxy " + e.message);
        }
    }

    void parseReceiverConfig(const YAML::Node &root)
    {
        const YAML::Node &receiverNode = root["receiver"];
        std::string key;

        try
        {
            parseStringVector(receiverIps, receiverNode, "ips");
            receiverPort = parseField<int>(receiverNode, "port");
            receiverKeysDir = parseField<std::string>(receiverNode, "keysDir");
            receiverShards = parseField<int>(receiverNode, "shards");
        }
        catch (const ConfigParseException &e)
        {
            throw ConfigParseException("Error parsing receiver " + e.message);
        }
    }

    void parseReplicaConfig(const YAML::Node &root)
    {
        const YAML::Node &replicaNode = root["replica"];
        std::string key;

        try
        {
            parseStringVector(replicaIps, replicaNode, "ips");
            replicaPort = parseField<int>(replicaNode, "port");
            replicaKeysDir = parseField<std::string>(replicaNode, "keysDir");
        }
        catch (const ConfigParseException &e)
        {
            throw ConfigParseException("Error parsing replica " + e.message);
        }
    }

    void parseConfig(const std::string &configFilename)
    {
        YAML::Node config;

        try
        {
            config = YAML::LoadFile(configFilename);
        }
        catch (const YAML::BadFile &e)
        {
            throw ConfigParseException("Error loading config file:" + e.msg + ".");
        }

        LOG(INFO) << "Using config:\n " << config;

        parseClientConfig(config);
        parseProxyConfig(config);
        parseReceiverConfig(config);
        parseReplicaConfig(config);
    }
};

#endif