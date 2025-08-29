#include "Agave.hpp"
#include "EventLoop.hpp"
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <iostream>
#include "Awaiters.hpp"
#include <queue>
#include <sys/eventfd.h>
// 协程 echo 会话
inline agave::AsyncAction echo_session(EventLoop *loop, int client_fd)
{
    char buf[1024];
    while (true)
    {
        ssize_t n = co_await RecvAwaiter{loop, client_fd, buf, sizeof(buf)};
        if (n == -2)
        {
            continue; // EAGAIN，挂起等
        }
        if (n < 0)
        {
            perror("recv error");
            ::close(client_fd);
            co_return;
        }
        if (n == 0)
        {
            ::close(client_fd);
            co_return;
        }
#ifndef iperf2
        ssize_t total = 0;
        while (total < n)
        {
            ssize_t w = co_await SendAwaiter{loop, client_fd, buf + total, (size_t)(n - total)};
            if (w == -2)
            {
                continue; // 等待可写
            }
            if (w < 0)
            {
                perror("send error");
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
struct Worker
{
    std::thread th;
    EventLoop loop;

    int wakeup_fd;
    std::mutex mtx;
    std::queue<int> new_clients;

    std::vector<agave::AsyncAction> tasks;

    Worker()
    {
        wakeup_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd < 0)
        {
            perror("eventfd");
            exit(1);
        }
        loop.add_fd(wakeup_fd, EPOLLIN);
        tasks.reserve(65536); // 防止 vector reallocation
    }

    void start()
    {
        th = std::thread([this]()
                         { loop.run_with_handler([this](int fd)
                                                 { this->handle_event(fd); }); });
    }

    void add_client(int client_fd)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            new_clients.push(client_fd);
        }
        uint64_t one = 1;
        ::write(wakeup_fd, &one, sizeof(one));
    }

private:
    void handle_event(int fd)
    {
        if (fd == wakeup_fd)
        {
            uint64_t cnt;
            ::read(wakeup_fd, &cnt, sizeof(cnt));

            std::queue<int> q;
            {
                std::lock_guard<std::mutex> lk(mtx);
                std::swap(q, new_clients);
            }
            while (!q.empty())
            {
                int client_fd = q.front();
                q.pop();

                loop.add_fd(client_fd, EPOLLIN);
                auto task = echo_session(&loop, client_fd);
                task.start();
                tasks.push_back(std::move(task));
            }
        }
    }
};
int main()
{
#ifdef iperf2
    std::cout << "iperf2 mode" << std::endl;
#endif
    int listen_fd = make_server_socket(8080);
    std::cout << "Echo server (multi-thread, eventfd) on 8080...\n";

    const int num_threads = std::thread::hardware_concurrency();
    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(num_threads);

    for (int i = 0; i < num_threads; i++)
    {
        workers.push_back(std::make_unique<Worker>());
        workers[i]->start();
    }

    int next = 0;
    while (true)
    {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int client_fd = ::accept4(listen_fd, (sockaddr *)&cli, &len, SOCK_NONBLOCK);
        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            else
            {
                perror("accept4");
                break;
            }
        }

        workers[next]->add_client(client_fd);
        next = (next + 1) % num_threads;
    }

    return 0;
}

#ifdef onethread
int main()
{
    int listen_fd = make_server_socket(8080);
    // std::cout << "Echo server on 8080...\n";

    static std::vector<agave::AsyncAction> tasks;

    auto &loop = EventLoop::instance();
    // 告诉 loop 怎么处理新连接
    loop.set_listen_fd(listen_fd, [](int cfd)
                       {
        auto action = echo_session(cfd);
        tasks.push_back(std::move(action)); });
    loop.add_fd(listen_fd, EPOLLIN); // 监听 accept 事件
    loop.run();                      // 阻塞式事件循环

    return 0;
}
#endif