#pragma once
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <coroutine>
#include <cstdint>
#include <vector>
#include <queue>
#include <unordered_map>
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <memory>
#include "IScheduler.hpp"

namespace agave
{

    inline int set_nonblock(int fd)
    {
        int f = fcntl(fd, F_GETFL, 0);
        return fcntl(fd, F_SETFL, f | O_NONBLOCK);
    }

    class EpollScheduler final : public IScheduler
    {
    public:
        EpollScheduler()
            : epfd_(::epoll_create1(EPOLL_CLOEXEC)),
              evfd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
              tfd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC))
        {
            assert(epfd_ >= 0 && evfd_ >= 0 && tfd_ >= 0);
            add_fd_(evfd_, EPOLLIN);
            add_fd_(tfd_, EPOLLIN);
        }

        ~EpollScheduler() override
        {
            ::close(tfd_);
            ::close(evfd_);
            ::close(epfd_);
        }

        void run() override
        {
            running_.store(true, std::memory_order_release);
            std::vector<epoll_event> evs(64);
            while (running_.load(std::memory_order_acquire))
            {
                // 如果 ready 队列不空，优先消费
                drain_ready_();

                int n = ::epoll_wait(epfd_, evs.data(), (int)evs.size(), 100); // 100ms tick for timer update
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                for (int i = 0; i < n; ++i)
                {
                    auto &e = evs[i];
                    if (e.data.u64 == (uint64_t)evfd_token_)
                    {
                        on_eventfd_();
                        continue;
                    }
                    if (e.data.u64 == (uint64_t)tfd_token_)
                    {
                        on_timerfd_();
                        continue;
                    }
                    on_fd_event_((void *)e.data.ptr, e.events);
                }
                // 处理到期定时（若用小根堆而非 timerfd，这里触发）
            }
            // 退出前把剩余 ready 全部跑完（可选）
            drain_ready_();
        }

        void stop() override
        {
            running_.store(false, std::memory_order_release);
            wake_();
        }

        void post(std::coroutine_handle<> h) override
        {
            {
                std::lock_guard<std::mutex> lk(rmt_mtx_);
                remote_ready_.push_back(h);
            }
            wake_();
        }

        void post_at(std::chrono::steady_clock::time_point tp, std::coroutine_handle<> h) override
        {
            // 简化：每次只设置最早一次到期到 timerfd；严格实现请用小根堆
            {
                std::lock_guard<std::mutex> lk(timer_mtx_);
                timers_.emplace(tp, h);
            }
            rearm_timerfd_();
        }

        // ---- 给 I/O awaiter 用的注册接口 ----
        struct Waiter
        {
            std::coroutine_handle<> h;
            uint32_t events; // EPOLLIN or EPOLLOUT
        };

        // 每个 fd、每种事件 EPOLLONESHOT 注册一个 waiter（简单稳定）
        void arm_fd(int fd, uint32_t ev, Waiter w)
        {
            set_nonblock(fd);
            // 存入 map，并以 ONESHOT 注册
            FdKey key{fd, ev};
            {
                std::lock_guard<std::mutex> lk(fd_mtx_);
                fd_waiters_[key] = w;
            }
            epoll_event ee{};
            ee.events = ev | EPOLLONESHOT | EPOLLET; // ET + ONESHOT
            ee.data.ptr = make_fd_token_(fd, ev);
            ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ee) == 0 ||
                ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ee);
        }

    private:
        // --- epoll 内核资源 ---
        int epfd_{-1}, evfd_{-1}, tfd_{-1};
        std::atomic<bool> running_{false};

        // --- ready 队列（本地 + 跨线程） ---
        std::vector<std::coroutine_handle<>> local_ready_;
        std::vector<std::coroutine_handle<>> remote_ready_;
        std::mutex rmt_mtx_;

        // --- 定时器（最简实现：小根堆 + timerfd，先用 timerfd 单点设置） ---
        using TP = std::chrono::steady_clock::time_point;
        struct TimerItem
        {
            TP tp;
            std::coroutine_handle<> h;
            bool operator<(const TimerItem &o) const { return tp > o.tp; }
        };
        std::priority_queue<TimerItem> timers_;
        std::mutex timer_mtx_;

        // --- fd → waiter ---
        struct FdKey
        {
            int fd;
            uint32_t ev;
            bool operator==(const FdKey &o) const noexcept { return fd == o.fd && ev == o.ev; }
        };
        struct FdKeyHash
        {
            size_t operator()(const FdKey &k) const noexcept { return (size_t)k.fd * 131u + k.ev; }
        };
        std::unordered_map<FdKey, Waiter, FdKeyHash> fd_waiters_;
        std::mutex fd_mtx_;

        // --- token for eventfd/timerfd ---
        static constexpr int evfd_token_ = -1;
        static constexpr int tfd_token_ = -2;
        void *make_fd_token_(int fd, uint32_t ev)
        { // 指针打包（保证唯一即可）
            // 用 malloc 分配一个小对象存 fd/ev，释放在 on_fd_event_ 里
            auto *p = new FdKey{fd, ev};
            return p;
        }

        void add_fd_(int fd, uint32_t ev)
        {
            epoll_event ee{};
            ee.events = ev;
            ee.data.u64 = (fd == evfd_) ? (uint64_t)evfd_token_ : (fd == tfd_) ? (uint64_t)tfd_token_
                                                                               : 0;
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ee);
        }

        void wake_()
        {
            uint64_t one = 1;
            ::write(evfd_, &one, sizeof(one));
        }

        void drain_ready_()
        {
            // 先拿远端 ready
            {
                std::lock_guard<std::mutex> lk(rmt_mtx_);
                local_ready_.insert(local_ready_.end(), remote_ready_.begin(), remote_ready_.end());
                remote_ready_.clear();
            }
            // 消费本地 ready
            for (auto h : local_ready_)
                if (h && !h.done())
                    h.resume();
            local_ready_.clear();
        }

        void on_eventfd_()
        {
            uint64_t x;
            while (::read(evfd_, &x, sizeof(x)) > 0)
            {
            }
            drain_ready_();
        }

        void rearm_timerfd_()
        {
            std::lock_guard<std::mutex> lk(timer_mtx_);
            if (timers_.empty())
            {
                itimerspec its{};
                ::timerfd_settime(tfd_, 0, &its, nullptr);
                return;
            }
            auto now = std::chrono::steady_clock::now();
            auto dt = timers_.top().tp - now;
            if (dt < std::chrono::steady_clock::duration::zero())
                dt = std::chrono::milliseconds(0);
            itimerspec its{};
            its.it_value.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(dt).count();
            its.it_value.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count() % 1000000000LL;
            ::timerfd_settime(tfd_, 0, &its, nullptr);
        }

        void on_timerfd_()
        {
            uint64_t x;
            while (::read(tfd_, &x, sizeof(x)) > 0)
            {
            }
            auto now = std::chrono::steady_clock::now();
            std::vector<std::coroutine_handle<>> ready;
            {
                std::lock_guard<std::mutex> lk(timer_mtx_);
                while (!timers_.empty() && timers_.top().tp <= now)
                {
                    ready.push_back(timers_.top().h);
                    timers_.pop();
                }
            }
            for (auto h : ready)
                if (h)
                    local_ready_.push_back(h);
            rearm_timerfd_();
        }

        void on_fd_event_(void *token, uint32_t events)
        {
            // 拿出 fd/ev，查 waiter，恢复协程；由于 ONESHOT，需重新注册由 awaiter 完成
            std::unique_ptr<FdKey> key((FdKey *)token);
            Waiter w{};
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(fd_mtx_);
                auto it = fd_waiters_.find(*key);
                if (it != fd_waiters_.end())
                {
                    w = it->second;
                    fd_waiters_.erase(it);
                    found = true;
                }
            }
            if (found && w.h)
                local_ready_.push_back(w.h);
        }
    };

} // namespace agave
