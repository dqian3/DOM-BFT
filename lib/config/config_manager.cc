#include "config_manager.h"
#include <glog/logging.h>
#include <sstream>

namespace dombft {

// Static member initialization
std::unique_ptr<ConfigManager> ConfigManager::instance_ = nullptr;

void ConfigManager::initialize(const ProcessConfig &config)
{
    if (instance_) {
        LOG(WARNING) << "ConfigManager already initialized. Replacing existing instance.";
    }

    instance_ = std::unique_ptr<ConfigManager>(new ConfigManager());
    instance_->config_ = config;
    instance_->calculateDerivedValues();

    LOG(INFO) << "ConfigManager initialized: " << instance_->getConfigurationSummary();
}

ConfigManager &ConfigManager::getInstance()
{
    if (!instance_) {
        throw std::runtime_error("ConfigManager not initialized. Call initialize() first.");
    }
    return *instance_;
}

bool ConfigManager::isInitialized() { return instance_ != nullptr; }

void ConfigManager::reset() { instance_.reset(); }

void ConfigManager::calculateDerivedValues()
{
    numReplicas_ = config_.replicaIps.size();
    numClients_ = config_.clientIps.size();
    numProxies_ = config_.proxyIps.size();

    quorumSize_ = 2 * config_.f + 2 * config_.e + 1;
    superQuorumSize_ = 3 * config_.f + config_.e + 1;

    assert(numReplicas_ == 3 * config_.f + 2 * config_.e + 1);
}

std::string ConfigManager::getConfigurationSummary() const
{
    std::ostringstream oss;
    oss << "BFT Config: f=" << config_.f << ", e=" << config_.e << ", replicas=" << numReplicas_
        << ", clients=" << numClients_ << ", proxies=" << numProxies_ << ", transport=" << config_.transport
        << ", app=" << config_.appStr;
    return oss.str();
}

}   // namespace dombft