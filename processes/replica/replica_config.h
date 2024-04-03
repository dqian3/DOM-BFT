#include <glog/logging.h>
#include <stdint.h>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>

#include <gflags/gflags.h>

DECLARE_int32(replicaId);

struct ReplicaConfig
{
    // Own Config
    int replicaId;
    std::vector<std::string> replicaIps;
    std::string replicaKeysDir;
    std::string replicaKey;
    int replicaPort;

    std::vector<std::string> clientIps;
    int clientPort;
    std::string clientKeysDir;

    std::string proxyKeysDir;
    std::string receiverKeysDir;

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


        replicaId = FLAGS_replicaId;

        std::string key; // Keep track of current key for better error messages
        try
        {
            key = "replicaIps";
            for (uint32_t i = 0; i < config[key].size(); i++)
            {
                replicaIps.push_back(config[key][i].as<std::string>());
            }

            key = "replicaKeysDir";
            replicaKeysDir = config[key].as<std::string>();

            key = "replicaKey";
            replicaKey = config[key].as<std::string>();

            key = "replicaPort";
            replicaPort = config[key].as<int>();


            key = "clientIps";
            for (uint32_t i = 0; i < config[key].size(); i++)
            {
                clientIps.push_back(config[key][i].as<std::string>());
            }
            key = "clientPort";
            clientPort = config[key].as<int>();
            
            key = "clientKeysDir";
            clientKeysDir = config[key].as<std::string>();

            key = "proxyKeysDir";
            proxyKeysDir = config[key].as<std::string>();

            key = "receiverKeysDir";
            receiverKeysDir = config[key].as<std::string>();

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