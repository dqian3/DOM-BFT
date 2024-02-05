#ifndef ADDRESS_H
#define ADDRESS_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>
#include <string>

/**
 * Encapsualted IP addresses
 */

class Address
{
public:
    std::string ip_;
    int port_;
    struct sockaddr_in addr_;

    Address();
    Address(const Address &addr)
        : ip_(addr.ip_), port_(addr.port_)
    {
        memcpy(&addr_, &(addr.addr_), sizeof(struct sockaddr_in));
    }
    Address(const std::string &ip, const int port);
    ~Address();

    std::string GetIPAsString();
    int GetPortAsInt();
};

#endif