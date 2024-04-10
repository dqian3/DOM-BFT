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
    std::string replicaKeysDir;
    int replicaPort;

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
};