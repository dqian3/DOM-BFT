#include <glog/logging.h>
#include <stdint.h>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>

struct ProxyConfig
{
    // Own Config
    int proxyId;
    std::string proxyIp;
    int proxyNumShards;
    int proxyForwardPortBase;
    int proxyMeasurementPort;
    std::string proxyKey;

    // Measurement settings
    uint32_t initialOwd;
    uint32_t maxOwd;

    // Receiver
    std::vector<std::string> receiverIps;
    std::vector<int> receiverPorts;
    int receiverPort;
    int receiverShards;

    // Clients
    int numClients;

    // Parses yaml file configFilename and fills in fields of ProxyConfig
    // accordingly. Returns an error message or "" if there are no errors.
    std::string parseConfig(std::string configFilename)
    {
        YAML::Node config;
        try
        {
            config = YAML::LoadFile(configFilename);
        }
        catch (const YAML::BadFile &e)
        {
            return "Error loading config file:" + e.msg + ".";
        }
        LOG(INFO) << "Using config:\n " << config;

        std::string key; // Keep track of current key for better error messages
        try
        {
            key = "receiverIps";
            for (uint32_t i = 0; i < config[key].size(); i++)
            {
                receiverIps.push_back(config[key][i].as<std::string>());
            }
            key = "receiverShards";
            receiverShards = config[key].as<int>();
            key = "receiverPort";
            receiverPort = config[key].as<int>();
            // key = "receiverHmacPrefix";
            // receiverHmacPrefix = config[key].as<std::string>();

            key = "proxyId";
            proxyId = config[key].as<int>();
            key = "proxyIp";
            proxyIp = config[key].as<std::string>();
            key = "proxyNumShards";
            proxyNumShards = config[key].as<int>();
            key = "proxyForwardPortBase";
            proxyForwardPortBase = config[key].as<int>();
            key = "proxyMeasurementPort";
            proxyMeasurementPort = config[key].as<int>();
            key = "proxyKey";
            proxyKey = config[key].as<std::string>();

            key = "initialOwd";
            initialOwd = config[key].as<uint32_t>();
            key = "maxOwd";
            maxOwd = config[key].as<uint32_t>();

            key = "numClients";
            numClients = config[key].as<int>();
            return "";
        }
        catch (const YAML::BadConversion &e)
        {
            if (config[key])
            {
                return "Error parsing config field " + key + ": " + e.msg + ".";
            }
            else
            {
                return "Error parsing config field " + key + ": key not found.";
            }
        }
        catch (const std::exception &e)
        {
            return "Error parsing config field " + key + ": " + e.what() + ".";
        }
    }

    // Parses yaml file configFilename and fills in fields of ProxyConfig
    // accordingly. Returns an error message or "" if there are no errors.
    std::string parseUnifiedConfig(std::string configFilename, const size_t proxyId)
    {
        YAML::Node config;
        try
        {
            config = YAML::LoadFile(configFilename);
        }
        catch (const YAML::BadFile &e)
        {
            return "Error loading config file:" + e.msg + ".";
        }
        LOG(INFO) << "Using config:\n " << config;

        std::string key; // Keep track of current key for better error messages
        try
        {
            key = "receivers";
            for (const auto& receiverInfo : config[key]) {
                key = "receiverIp";
                receiverIps.push_back(receiverInfo[key].as<std::string>());
                key = "receiverPort";
                receiverPorts.push_back(receiverInfo[key].as<int>());
            }
            key = "receiverShards";
            receiverShards = config[key].as<int>();
            // key = "receiverHmacPrefix";
            // receiverHmacPrefix = config[key].as<std::string>();
            key = "proxies";
            for (const auto& proxyInfo : config[key]) {
                key = "proxyId";
                if (proxyId != proxyInfo[key].as<int>())
                    continue;
                
                key = "proxyIp";
                proxyIp = proxyInfo[key].as<std::string>();
                key = "proxyNumShards";
                proxyNumShards = proxyInfo[key].as<int>();
                key = "proxyForwardPortBase";
                proxyForwardPortBase = proxyInfo[key].as<int>();
                key = "proxyMeasurementPort";
                proxyMeasurementPort = proxyInfo[key].as<int>();
                key = "proxyKey";
                proxyKey = proxyInfo[key].as<std::string>();
            }

            key = "initialOwd";
            initialOwd = config[key].as<uint32_t>();
            key = "maxOwd";
            maxOwd = config[key].as<uint32_t>();
            key = "clients";
            numClients = config[key].size();
            return "";
        }
        catch (const YAML::BadConversion &e)
        {
            if (config[key])
            {
                return "Error parsing config field " + key + ": " + e.msg + ".";
            }
            else
            {
                return "Error parsing config field " + key + ": key not found.";
            }
        }
        catch (const std::exception &e)
        {
            return "Error parsing config field " + key + ": " + e.what() + ".";
        }
    }
};