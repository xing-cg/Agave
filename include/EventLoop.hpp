#pragma once
#include <sys/epoll.h>
#include <unistd.h>
#include <coroutine>
#include <unordered_map>
#include <iostream>
#include <arpa/inet.h>
#include <functional>

class EventLoop
{
public:
    EventLoop()
    {
        epfd = epoll_create1(0);
        if (epfd < 0)
        {
            perror("epoll_create1");
            throw std::runtime_error("epoll_create1 failed");
        }
    }

    ~EventLoop()
    {
        if (epfd >= 0)
            ::close(epfd);
    }

    void add_fd(int fd, uint32_t events)
    {
        epoll_event ev{};
        ev.data.fd = fd;
        ev.events = events;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            perror("epoll_ctl ADD");
        }
    }

    void register_fd(int fd, uint32_t events, std::coroutine_handle<> h)
    {
        epoll_event ev{};
        ev.data.fd = fd;
        ev.events = events;

        handlers[fd] = h;

        if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0)
        {
            if (errno == ENOENT)
            {
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
                {
                    perror("epoll_ctl ADD");
                }
            }
            else
            {
                perror("epoll_ctl MOD");
            }
        }
    }

    void run_with_handler(std::function<void(int)> extra_handler)
    {
        constexpr int MAX_EVENTS = 64;
        epoll_event events[MAX_EVENTS];

        while (true)
        {
            int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
            for (int i = 0; i < n; i++)
            {
                int fd = events[i].data.fd;

                if (extra_handler)
                {
                    extra_handler(fd);
                }

                auto it = handlers.find(fd);
                if (it != handlers.end())
                {
                    auto h = it->second;
                    handlers.erase(it);
                    if (h && !h.done())
                    {
                        h.resume();
                    }
                }
            }
        }
    }

private:
    int epfd;
    std::unordered_map<int, std::coroutine_handle<>> handlers;
};