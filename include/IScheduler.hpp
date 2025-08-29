struct IScheduler
{
    virtual ~IScheduler() = default;
    virtual void run() = 0;   // 需要事件循环的后端实现（epoll/loop），线程池可空实现
    virtual void stop() = 0;
    virtual void post(std::coroutine_handle<>) = 0; // 尽快恢复此协程
    virtual void post_at(std::chrono::steady_clock::time_point, std::coroutine_handle<>) = 0; // 定时恢复
};

struct resume_on
{
    IScheduler* sch{};
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const { sch->post(h); }
    void await_resume() const noexcept {}
};
