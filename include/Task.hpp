#pragma once
#include <coroutine>
#include <exception>
#include <utility>

namespace agave
{

    template <class T = void>
    class task;

    namespace detail
    {
        template <class T>
        struct task_promise
        {
            T value_;
            std::exception_ptr ep_;
            std::coroutine_handle<> cont_;

            task<T> get_return_object() noexcept;
            std::suspend_always initial_suspend() noexcept { return {}; }
            struct final_awaitable
            {
                bool await_ready() noexcept { return false; }
                template <class P>
                void await_suspend(std::coroutine_handle<P> h) noexcept
                {
                    if (h.promise().cont_)
                        h.promise().cont_.resume();
                }
                void await_resume() noexcept {}
            };
            final_awaitable final_suspend() noexcept { return {}; }
            void unhandled_exception() noexcept { ep_ = std::current_exception(); }
            template <class U>
            void return_value(U &&v) noexcept(std::is_nothrow_assignable_v<T &, U &&>) { value_ = std::forward<U>(v); }
        };

        template <>
        struct task_promise<void>
        {
            std::exception_ptr ep_;
            std::coroutine_handle<> cont_;
            task<void> get_return_object() noexcept;
            std::suspend_always initial_suspend() noexcept { return {}; }
            struct final_awaitable
            {
                bool await_ready() noexcept { return false; }
                template <class P>
                void await_suspend(std::coroutine_handle<P> h) noexcept
                {
                    if (h.promise().cont_)
                        h.promise().cont_.resume();
                }
                void await_resume() noexcept {}
            };
            final_awaitable final_suspend() noexcept { return {}; }
            void unhandled_exception() noexcept { ep_ = std::current_exception(); }
            void return_void() noexcept {}
        };
    } // detail

    template <class T>
    class task
    {
    public:
        using promise_type = detail::task_promise<T>;
        using handle = std::coroutine_handle<promise_type>;
        explicit task(handle h) : h_(h) {}
        task(task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
        ~task()
        {
            if (h_)
                h_.destroy();
        }

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> cont)
        {
            h_.promise().cont_ = cont;
            h_.resume();
        }
        T await_resume()
        {
            if (h_.promise().ep_)
                std::rethrow_exception(h_.promise().ep_);
            return std::move(h_.promise().value_);
        }

    private:
        handle h_{};
    };

    template <>
    class task<void>
    {
    public:
        using promise_type = detail::task_promise<void>;
        using handle = std::coroutine_handle<promise_type>;
        explicit task(handle h) : h_(h) {}
        task(task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
        ~task()
        {
            if (h_)
                h_.destroy();
        }

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> cont)
        {
            h_.promise().cont_ = cont;
            h_.resume();
        }
        void await_resume()
        {
            if (h_.promise().ep_)
                std::rethrow_exception(h_.promise().ep_);
        }

    private:
        handle h_{};
    };

    namespace detail
    {
        template <class T>
        inline task<T> task_promise<T>::get_return_object() noexcept
        {
            return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
        }
        inline task<void> task_promise<void>::get_return_object() noexcept
        {
            return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
        }
    } // detail

    // fire-and-forget：把协程提交到调度器
    template <class Awaitable>
    inline void co_spawn(IScheduler &sched, Awaitable &&aw)
    {
        struct starter
        {
            struct awaiter
            {
                IScheduler &s;
                Awaitable aw;
                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<> h) { s.post(h); }
                void await_resume() {}
            };
            static task<void> run(IScheduler &s, Awaitable aw)
            {
                co_await awaiter{s, std::move(aw)};
                co_return;
            }
        };
        (void)starter::run(sched, std::forward<Awaitable>(aw));
    }

}; // namespace agave