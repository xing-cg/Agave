#include "Agave.hpp"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <thread>

int make_server_socket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(fd, (sockaddr *)&addr, sizeof(addr));
    listen(fd, SOMAXCONN);
    return fd;
}

// 每个 client 连接，开一个线程，用阻塞的 read/write 实现 echo
void echo_session(int client_fd) {
    char buf[1024];
    while (true) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            ::close(client_fd);
            return;
        }
        ::send(client_fd, buf, n, 0);
    }
}

int main() {
    int listen_fd = make_server_socket(8081);
    std::cout << "Traditional echo server on 8081...\n";

    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int client_fd = ::accept(listen_fd, (sockaddr *)&cli, &len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        // 每个连接开一个 std::thread（最传统模式）
        std::thread([client_fd]() {
            echo_session(client_fd);
        }).detach();
    }

    return 0;
}
