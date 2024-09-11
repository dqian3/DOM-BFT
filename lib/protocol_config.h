#ifndef CONFIG_H
#define CONFIG_H

// Compile time configs

// In general, these are for changing major changes to the behavior of the protocols
// rather than for tunable parameters such as timeouts or changeable information such
// as IPs and ports. Generally these will be used for experiments

// TOOD, move these to yaml
#define USE_PROXY 1

#define FABRIC_CRYPTO 0

#define MAX_SPEC_HIST       1000
#define CHECKPOINT_INTERVAL 100

#endif
