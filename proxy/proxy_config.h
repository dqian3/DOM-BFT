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
    int proxyMeasurmentPort;

    // Measurement settings
    uint32_t initialOwd;
    uint32_t maxOwd;

    // Receiver
    std::vector<std::string> receiverIps;
    int receiverPort;
    int receiverShards;
    std::string receiverHMACPrefix;

    // Clients
    int numClients;
    std::string clientPubKeyPrefix;

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
            key = "receiverHmacPrefix";
            receiverHMACPrefix = config[key].as<std::string>();

            key = "proxyId";
            proxyId = config[key].as<int>();
            key = "proxyIp";
            proxyIp = config[key].as<std::string>();
            key = "proxyNumShards";
            proxyNumShards = config[key].as<int>();
            key = "proxyForwardPortBase";
            proxyForwardPortBase = config[key].as<int>();
            key = "proxyMeasurmentPort";
            proxyMeasurmentPort = config[key].as<int>();

            key = "initialOwd";
            initialOwd = config[key].as<uint32_t>();
            key = "maxOwd";
            maxOwd = config[key].as<uint32_t>();

            key = "numClients";
            numClients = config[key].as<int>();
            key = "clientPubKeyPrefix";
            clientPubKeyPrefix = config[key].as<std::string>();

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