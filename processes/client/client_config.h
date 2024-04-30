#include <glog/logging.h>
#include <stdint.h>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>

struct ClientConfig
{
    int clientId;
    std::string clientIp;
    std::string clientKey;
    int maxInFlight;

    int clientPort;
    int proxyPortBase;

    std::vector<std::string> proxyIps;

    std::vector<std::string> replicaIps;
    std::vector<unsigned int> replicaPorts;
    std::string replicaKeysDir;
    int replicaPort;

    

    // Parses yaml file configFilename and fills in fields of ProxyConfig
    // accordingly. Returns an error message or "" if there are no errors.
    //TODO: make this a static method so we can call it from class instead of instance.
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
            key = "clientId";
            clientId = config[key].as<int>();
            key = "clientIp";
            clientIp = config[key].as<std::string>();
            key = "clientKey";
            clientKey = config[key].as<std::string>();
            key = "clientPort";
            clientPort = config[key].as<int>();

            key = "maxInFlight";
            maxInFlight = config[key].as<int>();

            key = "proxyPortBase";
            proxyPortBase = config[key].as<int>();


            // key = "writeRatio";
            // writeRatio = config[key].as<double>();
            // key = "requestRetryTimeoutUs";
            // requestRetryTimeoutUs = config[key].as<int>();

            // key = "requestRetryTimeoutUs";
            // requestRetryTimeoutUs = config[key].as<int>();

            key = "proxyIps";
            for (uint32_t i = 0; i < config[key].size(); i++)
            {
                proxyIps.push_back(config[key][i].as<std::string>());
            }

            key = "replicaIps";
            for (uint32_t i = 0; i < config[key].size(); i++)
            {
                replicaIps.push_back(config[key][i].as<std::string>());
            }

            key = "replicaPort";
            replicaPort = config[key].as<int>();
 
            key = "replicaKeysDir";
            replicaKeysDir = config[key].as<std::string>();


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
    //TODO: make this a static method so we can call it from class instead of instance.
    std::string parseUnifiedConfig(std::string configFilename, const size_t clientId)
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
        this->clientId = clientId;
        std::string key; // Keep track of current key for better error messages
        try
        {
            // Assuming that the clients are ordered by clientId in the yaml
            key = "clients";
            const auto& clientInfo = config[key][clientId];
            key = "clientId";
            int id = clientInfo["clientId"].as<int>();
            if (id == clientId) {
                key = "clientIp";
                this->clientIp = clientInfo[key].as<std::string>();
                key = "clientKey";
                this->clientKey = clientInfo[key].as<std::string>();
                key = "clientPort";
                this->clientPort = clientInfo[key].as<int>();
            } else {
                return "Error parsing config file: The clients should be in the increasing order of clientIds";
            }

            key = "maxInFlight";
            maxInFlight = config[key].as<int>();

            for (const auto& proxyInfo : config["proxies"]) {
                key = "proxyId";
                int id = proxyInfo[key].as<int>();
                //TODO: check if each proxy should have different proxyPortBase
                // i.e., should this also be a vector in the client config.
                if (id == 0) {
                    key = "proxyForwardPortBase";
                    proxyPortBase = proxyInfo[key].as<int>();
                }
                key = "proxyIp";
                proxyIps.push_back(proxyInfo[key].as<std::string>());
            }


            // key = "writeRatio";
            // writeRatio = config[key].as<double>();
            // key = "requestRetryTimeoutUs";
            // requestRetryTimeoutUs = config[key].as<int>();

            // key = "requestRetryTimeoutUs";
            // requestRetryTimeoutUs = config[key].as<int>();

            for (const auto& replicaInfo : config["replicas"]) {
                key = "replicaIp";
                replicaIps.push_back(replicaInfo[key].as<std::string>());
                key = "replicaPort";
                replicaPorts.push_back(replicaInfo[key].as<unsigned int>());
            }
 
            key = "replicaKeysDir";
            replicaKeysDir = config[key].as<std::string>();


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