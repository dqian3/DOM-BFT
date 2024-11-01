#include "config_util.h"

#include <glog/logging.h>

using namespace std;

void addAddrPairs(
    vector<pair<Address, Address>> &pairs, const std::string &myIp, uint32_t myBasePort,
    const std::vector<std::string> theirIps, int theirPort
)
{
    for (uint32_t i = 0; i < theirIps.size(); i++) {
        pairs.push_back({Address(myIp, myBasePort + i), Address(theirIps[i], theirPort)});
    }
}

void addAddrPairsToClient(
    vector<pair<Address, Address>> &pairs, const std::string &myIp, uint32_t myBasePort,
    const std::vector<std::string> theirIps, int theirPort, int portRangeWidth
)
{
    for (uint32_t i = 0; i < theirIps.size(); i++) {
        pairs.push_back({Address(myIp, myBasePort + i), Address(theirIps[i], theirPort + i * portRangeWidth)});
    }
}

vector<pair<Address, Address>> getClientAddrs(ProcessConfig config, uint32_t id)
{
    // TODO modify config to specify that these are base ports, and not
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    uint32_t clientBase = config.clientPort + id * portRangeWidth;

    // 1. clientBase + (clientId  * (numProxies + numReplicas) + replicaId <==> replicaBase + clientId
    std::string clientIp = config.clientIps[id];
    uint32_t replicaPort = config.replicaPort + id;
    addAddrPairs(ret, clientIp, clientBase, config.replicaIps, replicaPort);

    // 2. clientBase + (clientId  * (numProxies + numReplicas) + nReplicas + proxyId <==> proxyForwardBase + clientId
    clientBase += config.replicaIps.size();
    uint32_t proxyPort = config.proxyForwardPort + id;
    addAddrPairs(ret, clientIp, clientBase, config.proxyIps, proxyPort);

    return ret;
}

vector<pair<Address, Address>> getProxyAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    // 2. clientBase + (clientId  * (numProxies + numReplicas) + nReplicas + proxyId <==> proxyForwardBase + clientId
    std::string proxyIp = config.proxyIps[id];
    uint32_t proxyBase = config.proxyForwardPort;
    uint32_t clientPort = config.clientPort + id;
    addAddrPairsToClient(ret, proxyIp, proxyBase, config.clientIps, clientPort, portRangeWidth);

    // 3. proxyForwardBase + nClients + receiverId <==> receiverBase + proxyId
    proxyBase += config.clientIps.size();
    uint32_t receiverPort = config.receiverPort + id;
    addAddrPairs(ret, proxyIp, proxyBase, config.receiverIps, receiverPort);

    // 4. proxyMeasurmentBase + receiverId <==> receiverBase + proxyId + numProxies
    proxyBase = config.proxyMeasurementPort;
    receiverPort = config.receiverPort + config.proxyIps.size() + id;
    addAddrPairs(ret, proxyIp, proxyBase, config.receiverIps, receiverPort);

    return ret;
}

vector<pair<Address, Address>> getReceiverAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    // 3. proxyForwardBase + nClients + receiverId <==> receiverBase + proxyId
    std::string receiverIp = config.receiverIps[id];
    int receiverBase = config.receiverPort;

    uint32_t proxyBase = config.proxyForwardPort + config.clientIps.size();
    addAddrPairs(ret, receiverIp, receiverBase, config.proxyIps, proxyBase);
    // 4. proxyMeasurmentBase + receiverId <==> receiverBase + proxyId + numProxies
    receiverBase += config.proxyIps.size();
    proxyBase = config.proxyMeasurementPort;
    addAddrPairs(ret, receiverIp, receiverBase, config.proxyIps, proxyBase);

    // 5a. Each replica/receiver own address (i.e. loopback for local exp.)
    //      receiverBase + numProxies * 2 <==> replicaBase + numClients
    // 5b. Each replica/receiver own machine
    //      (127.0.0.1) receiverBase + numProxies * 2 <==> (127.0.0.2) replicaBase + numClients
    receiverBase += config.proxyIps.size();
    uint32_t replicaBase = config.replicaPort + config.clientIps.size();

    if (config.receiverLocal) {
        // 5b. above
        // TODO use IPC instead of localhost?
        addAddrPairs(ret, "127.0.0.1", receiverBase, {"127.0.0.2"}, replicaBase);
    } else {
        // Only connect to corresponding replica
        addAddrPairs(ret, receiverIp, receiverBase, {config.replicaIps[id]}, replicaBase);
    }

    return ret;
}

vector<pair<Address, Address>> getReplicaAddrs(ProcessConfig config, uint32_t id)
{
    vector<pair<Address, Address>> ret;
    uint32_t portRangeWidth = (config.proxyIps.size() + config.replicaIps.size());

    // 1. clientBase + (clientId  * (numProxies + numReplicas) + replicaId <==> replicaBase + clientId
    std::string replicaIp = config.replicaIps[id];
    uint32_t replicaBase = config.replicaPort;
    uint32_t clientPort = config.clientPort + id;
    addAddrPairsToClient(ret, replicaIp, replicaBase, config.clientIps, clientPort, portRangeWidth);

    // 5a. Each replica/receiver own address (i.e. loopback for local exp.)
    //      receiverBase + numProxies * 2 <==> replicaBase + numClients
    // 5b. Each replica/receiver own machine
    //      (127.0.0.1) receiverBase + numProxies * 2 <==> (127.0.0.2) replicaBase + numClients

    uint32_t replicaPort = config.replicaPort + config.clientIps.size();
    uint32_t receiverPort = config.receiverPort + 2 * config.proxyIps.size();

    if (config.receiverLocal) {
        // 5b. above
        // TODO use IPC instead of localhost?
        addAddrPairs(ret, "127.0.0.2", replicaPort, {"127.0.0.1"}, receiverPort);
    } else {
        // Only connect to corresponding receiver
        addAddrPairs(ret, replicaIp, replicaPort, {config.receiverIps[id]}, receiverPort);
    }

    // Replica to replicas
    replicaBase = config.replicaPort + config.clientIps.size() + 1;

    // Each replica just uses (base + i) to connect with replica i
    for (uint32_t i = 0; i < config.replicaIps.size(); i++) {
        ret.push_back({Address(replicaIp, replicaBase + i), Address(config.replicaIps[i], replicaBase + id)});
    }

    return ret;
}
