class ThreadPoolScheduler final : public IScheduler
{
    BJobScheduler& pool_; // 你现有的线程池/任务系统
public:
    explicit ThreadPoolScheduler(BJobScheduler& p) : pool_(p) {}
    void run() override {}                  // 线程池无需自旋
    void stop() override {}                 // 如需，转调 pool_.stop()
    void post(std::coroutine_handle<> h) override
    {
        pool_.enqueue([h]{ if (h && !h.done()) h.resume(); });
    }
    void post_at(std::chrono::steady_clock::time_point tp, std::coroutine_handle<> h) override
    {
        // 若 BJobScheduler 有延时/定时 API → 直接映射；
        // 若没有 → 用一个轻量 timerfd/小根堆/或单线程timer线程，到期后再 post(h)。
        schedule_after(tp, [this,h]{ post(h); });
    }
};
