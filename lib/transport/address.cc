#include "lib/transport/address.h"

Address::Address()
    : ip_("")
    , port_(-1)
{
    bzero(&addr_, sizeof(addr_));
}

Address::Address(const std::string &ip, const int port)
    : ip_(ip)
    , port_(port)
{
    bzero(&addr_, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = inet_addr(ip.c_str());
}

Address::Address(struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));

    ip_ = std::string(client_ip);
    port_ = ntohs(addr->sin_port);

    memcpy(&addr_, &addr, sizeof(struct sockaddr_in));
}

Address::~Address() {}

std::string Address::ip() const { return ip_; }

int Address::port() const { return port_; }

bool Address::operator==(const Address &other) const { return ip_ == other.ip_ && port_ == other.port_; }

std::ostream &operator<<(std::ostream &os, const Address &address)
{
    os << address.ip() << ":" << address.port();
    return os;
}