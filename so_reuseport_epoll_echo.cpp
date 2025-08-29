#include "Agave.hpp"
#include "EventLoop.hpp"
#include "Awaiters.hpp"
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <iostream>
#include <thread>
#include <vector>
// 协程 echo 会话
agave::AsyncAction echo_session(EventLoop *loop, int client_fd)
{
    char buf[1024];
    while (true)
    {
        ssize_t n = co_await RecvAwaiter{loop, client_fd, buf, sizeof(buf)};
        if (n == -2)
            continue; // EAGAIN
        if (n <= 0)
        {
            ::close(client_fd);
            co_return;
        }
#ifndef iperf2
        ssize_t total = 0;
        while (total < n)
        {
            ssize_t w = co_await SendAwaiter{
                loop,
                client_fd,
                buf + total,
                static_cast<size_t>(n - total)};
            if (w == -2)
                continue; // 等待可写
            if (w <= 0)
            {
                ::close(client_fd);
                co_return;
            }
            total += w;
        }
#endif
    }
}

int make_server_socket(uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        perror("socket");
        exit(1);
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)); // 关键！

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(fd, SOMAXCONN) < 0)
    {
        perror("listen");
        exit(1);
    }
    return fd;
}

struct Worker
{
    std::thread th;
    EventLoop loop;
    int listen_fd;
    std::vector<agave::AsyncAction> tasks;

    Worker(uint16_t port)
    {
        listen_fd = make_server_socket(port);
        loop.add_fd(listen_fd, EPOLLIN);
        tasks.reserve(65536);
    }

    void start()
    {
        th = std::thread([this]()
                         { this->loop.run_with_handler([this](int fd)
                                                       {
                if (fd == listen_fd) {
                    while (true) {
                        sockaddr_in cli{};
                        socklen_t len = sizeof(cli);
                        int client_fd = ::accept4(listen_fd, (sockaddr *)&cli, &len, SOCK_NONBLOCK);
                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            perror("accept4");
                            break;
                        }
                        loop.add_fd(client_fd, EPOLLIN);
                        auto task = echo_session(&loop, client_fd);
                        task.start();
                        tasks.push_back(std::move(task));
                    }
                } }); });
    }
};

int main()
{
#ifdef iperf2
    std::cout << "iperf2 mode" << std::endl;
#endif
    const int num_threads = std::thread::hardware_concurrency();
    std::cout << "Echo server (SO_REUSEPORT) on 8080, threads=" << num_threads << "...\n";

    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(num_threads);

    for (int i = 0; i < num_threads; i++)
    {
        workers.push_back(std::make_unique<Worker>(8080));
        workers[i]->start();
    }

    for (auto &w : workers)
    {
        if (w->th.joinable())
            w->th.join();
    }
    return 0;
}
