#include <glog/logging.h>
#include <stdint.h>
#include <yaml-cpp/yaml.h>
#include <string>
#include <vector>

struct ReceiverConfig
{
    // Own Config
    int receiverId;
    std::string receiverIp;
    int receiverPort;
    std::string receiverKey;

    // Proxy
    int numProxies;
    int proxyMeasurementPort;
    std::string proxyPubKeyPrefix;

    // Communication to replicas
    bool ipcReplica = false;
    std::string ipcName = "";

    // Set of replicaIps to forward to if not over ipc, NOT all replicas
    // TODO for now we just configure this statically, in the future it perhaps should be dynamic
    std::vector<std::string> replicaIps;
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
            key = "receiverId";
            receiverId = config[key].as<int>();
            key = "receiverIp";
            receiverIp = config[key].as<std::string>();
            key = "receiverPort";
            receiverPort = config[key].as<int>();
            key = "receiverKey";
            receiverKey = config[key].as<std::string>();

            key = "numProxies";
            numProxies = config[key].as<int>();
            key = "proxyMeasurementPort";
            proxyMeasurementPort = config[key].as<int>();
            key = "proxyPubKeyPrefix";
            proxyPubKeyPrefix = config[key].as<std::string>();

            key = "ipcReplica";
            if (config[key])
            {
                ipcReplica = config[key].as<bool>();
                key = "ipcName";
                ipcName = config[key].as<std::string>();
            }

            key = "replicaIps";
            for (uint32_t i = 0; i < config[key].size(); i++)
            {
                replicaIps.push_back(config[key][i].as<std::string>());
            }

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