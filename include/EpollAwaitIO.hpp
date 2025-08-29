#pragma once
#include "EpollScheduler.hpp"
#include <sys/epoll.h>
#include <coroutine>

namespace agave
{

    // 等待 fd 可读
    struct readable
    {
        EpollScheduler *sched{};
        int fd{-1};

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const
        {
            sched->arm_fd(fd, EPOLLIN, EpollScheduler::Waiter{h, EPOLLIN});
        }
        void await_resume() const noexcept {}
    };

    // 等待 fd 可写
    struct writable
    {
        EpollScheduler *sched{};
        int fd{-1};

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const
        {
            sched->arm_fd(fd, EPOLLOUT, EpollScheduler::Waiter{h, EPOLLOUT});
        }
        void await_resume() const noexcept {}
    };

    // 睡眠/定时
    struct sleep_for
    {
        EpollScheduler *sched{};
        std::chrono::steady_clock::duration d{};
        bool await_ready() const noexcept { return d <= std::chrono::steady_clock::duration::zero(); }
        void await_suspend(std::coroutine_handle<> h) const
        {
            sched->post_at(std::chrono::steady_clock::now() + d, h);
        }
        void await_resume() const noexcept {}
    };

} // namespace agave
