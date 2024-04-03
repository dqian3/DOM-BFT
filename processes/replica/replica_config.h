#include <glog/logging.h>
#include <stdint.h>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>

struct ReplicaConfig
{
    // Own Config
    int replicaId;
    std::string replicaIp;
    std::string replicaKey;
    int replicaPort;

    std::vector<std::string> clientIps;
    int clientPort;
    std::string clientKeysDir;

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
            key = "replicaId";
            replicaId = config[key].as<int>();

            key = "replicaIp";
            replicaIp = config[key].as<std::string>();

            key = "replicaKey";
            replicaKey = config[key].as<std::string>();

            key = "replicaPort";
            replicaPort = config[key].as<int>();

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