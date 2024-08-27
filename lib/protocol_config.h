#ifndef CONFIG_H
#define CONFIG_H

// Compile time configs

// In general, these are for changing major changes to the behavior of the protocols
// rather than for tunable parameters such as timeouts or changeable information such
// as IPs and ports. Generally these will be used for experiments

// TOOD, move these to yaml

#define DOMBFT 0
#define PBFT   1
#define ZYZ    2

#define PROTOCOL  DOMBFT
#define USE_PROXY (1 && (PROTOCOL == DOMBFT))

#define FABRIC_CRYPTO 0

#define MAX_SPEC_HIST 100

#endif
