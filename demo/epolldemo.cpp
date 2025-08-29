#include <iostream>
#include <coroutine>
#include <vector>
#include <mutex>
#include <queue>
#include <atomic>
#include <thread>
#include <future>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <errno.h>

// 简单的协程任务类型
struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// 全局事件循环
class EventLoop {
public:
    EventLoop() {
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            throw std::runtime_error("epoll_create1 failed: " + std::string(strerror(errno)));
        }
        
        // 启动事件循环线程
        loop_thread = std::thread([this] { run(); });
    }
    
    ~EventLoop() {
        stop_flag = true;
        if (loop_thread.joinable()) loop_thread.join();
        close(epoll_fd);
    }
    
    void add_fd(int fd, uint32_t events, std::coroutine_handle<> handle) {
        epoll_event event{};
        event.events = events;
        event.data.ptr = handle.address();
        
        std::lock_guard<std::mutex> lock(epoll_mutex);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
            throw std::runtime_error("epoll_ctl ADD failed: " + std::string(strerror(errno)) + 
                                     " (fd=" + std::to_string(fd) + ")");
        }
    }
    
    void remove_fd(int fd) {
        std::lock_guard<std::mutex> lock(epoll_mutex);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
            // 即使删除失败，我们仍然继续，因为fd可能已经关闭
            std::cerr << "Warning: epoll_ctl DEL failed: " << strerror(errno) 
                      << " (fd=" << fd << ")" << std::endl;
        }
    }

private:
    void run() {
        epoll_event events[64];
        
        while (!stop_flag) {
            int num_events = epoll_wait(epoll_fd, events, 64, 100);
            if (num_events == -1) {
                if (errno == EINTR) continue; // 被信号中断，继续等待
                perror("epoll_wait failed");
                break;
            }
            
            for (int i = 0; i < num_events; ++i) {
                auto handle = std::coroutine_handle<>::from_address(events[i].data.ptr);
                if (handle) handle.resume();
            }
        }
    }
    
    int epoll_fd;
    std::thread loop_thread;
    std::mutex epoll_mutex;
    std::atomic<bool> stop_flag{false};
};

// 全局事件循环实例
static EventLoop global_event_loop;

// 真正的异步等待器
class AsyncAwaiter {
public:
    AsyncAwaiter(int fd, uint32_t events) : fd(fd), events(events) {}
    
    bool await_ready() const noexcept {
        return false; // 总是挂起
    }
    
    void await_suspend(std::coroutine_handle<> handle) const {
        global_event_loop.add_fd(fd, events, handle);
    }
    
    void await_resume() noexcept {
        global_event_loop.remove_fd(fd);
    }

private:
    int fd;
    uint32_t events;
};

// 线程池用于CPU密集型任务
class ThreadPool {
public:
    ThreadPool(size_t threads = std::thread::hardware_concurrency()) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] {
                            return stop || !tasks.empty();
                        });
                        
                        if (stop && tasks.empty()) return;
                        
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }
    
    template<class F>
    auto enqueue(F&& f) -> std::future<decltype(f())> {
        using return_type = decltype(f());
        
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::forward<F>(f)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers)
            if (worker.joinable()) worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;
};

// 全局线程池
static ThreadPool global_thread_pool(2); // 使用2个线程

// 改造后的 co_await 支持
template<typename T>
struct FutureAwaiter {
    std::future<T> future;
    
    bool await_ready() const {
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    
    void await_suspend(std::coroutine_handle<> handle) {
        // 在线程池中等待future完成
        global_thread_pool.enqueue([this, handle] {
            future.wait();
            handle.resume();
        });
    }
    
    T await_resume() {
        return future.get();
    }
};

template<typename T>
FutureAwaiter<T> operator co_await(std::future<T> future) {
    return FutureAwaiter<T>{std::move(future)};
}

// 设置文件描述符为非阻塞模式
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("fcntl F_GETFL failed: " + std::string(strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl F_SETFL failed: " + std::string(strerror(errno)));
    }
}

// 使用示例
Task example_coroutine() {
    try {
        // 创建一个临时文件用于演示
        const char* filename = "test_file.txt";
        int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            throw std::runtime_error("open failed: " + std::string(strerror(errno)));
        }
        
        // 设置非阻塞模式
        set_nonblocking(fd);
        
        // 写入一些数据
        const char* data = "Hello, C++20 Coroutines!";
        ssize_t written = write(fd, data, strlen(data));
        if (written == -1) {
            throw std::runtime_error("write failed: " + std::string(strerror(errno)));
        }
        
        // 移动文件指针到开头
        if (lseek(fd, 0, SEEK_SET) == -1) {
            throw std::runtime_error("lseek failed: " + std::string(strerror(errno)));
        }
        
        std::cout << "Waiting for file to be readable..." << std::endl;
        
        // 等待文件可读
        co_await AsyncAwaiter(fd, EPOLLIN);
        
        char buffer[256];
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read == -1) {
            throw std::runtime_error("read failed: " + std::string(strerror(errno)));
        }
        
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            std::cout << "Read from file: " << buffer << std::endl;
        }
        
        close(fd);
        unlink(filename);
        
        // CPU密集型任务（使用线程池）
        auto cpu_task = [] {
            // 模拟CPU密集型计算
            double sum = 0.0;
            for (int i = 0; i < 1000000; i++) {
                sum += std::sqrt(i);
            }
            return sum;
        };
        
        std::cout << "Starting CPU-intensive task..." << std::endl;
        auto future = global_thread_pool.enqueue(cpu_task);
        auto result = co_await std::move(future);
        
        std::cout << "CPU task result: " << result << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Exception in coroutine: " << e.what() << std::endl;
    }
}

int main() {
    try {
        // 启动示例协程
        example_coroutine();
        
        // 主线程需要等待一段时间，让协程有机会执行
        std::cout << "Main thread waiting for coroutines to complete..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        std::cout << "Program completed successfully." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Exception in main: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}