#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

#include <iostream>
#include <cstring>


// Small program to test pair protocol behavior
// Try running
// ./bazel-bin/test/sandbox_nng "tcp://127.0.0.1:4000" "tcp://127.0.0.2:4000"
// ./bazel-bin/test/sandbox_nng "tcp://127.0.0.2:4000" "tcp://127.0.0.1:4000"
// in separate windows. Both will send and recv messages!

int main(int argc, char *argv[])
{
    nng_socket sock;
    nng_pair_open(&sock);
    nng_listen(sock, argv[1], NULL, 0);
    nng_dial(sock, argv[2], NULL, 0);

    
    char recv_buf[1000];
    size_t recv_size;


    const char *msg = "Hello!";
    nng_send(sock, (void *) msg, strlen(msg) + 1, 0);

    while (true) {
        nng_recv(sock, recv_buf, &recv_size, 0);
        printf("Received: %s\n", recv_buf);
    }
}

