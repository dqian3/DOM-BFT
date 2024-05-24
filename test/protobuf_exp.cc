#include "proto/dombft_proto.pb.h"

#include <iostream>

using namespace dombft::proto;

int main()
{
    Reply r;
    r.set_client_id(50);
    Cert c;

    (*c.add_replies()) = r;


    std::cout << c.replies().size() << "\n";
    std::cout << c.replies()[0].client_id() << "\n"; 
}