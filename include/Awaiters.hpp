#pragma once
struct RecvAwaiter
{
    EventLoop *loop;
    int fd;
    char *buf;
    size_t len;
    ssize_t nread;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h)
    {
        loop->register_fd(fd, EPOLLIN, h);
    }
    ssize_t await_resume()
    {
        nread = ::recv(fd, buf, len, 0);
        if (nread == 0)
        {
            return 0; // client close
        }
        if (nread < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return -2; // 继续等
            }
            return -1; // 真错误
        }
        return nread;
    }
};

struct SendAwaiter
{
    EventLoop *loop;
    int fd;
    const char *buf;
    size_t len;
    ssize_t nsent;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h)
    {
        loop->register_fd(fd, EPOLLOUT, h);
    }
    ssize_t await_resume()
    {
        nsent = ::send(fd, buf, len, 0);
        if (nsent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return -2; // 等待下次可写
            }
            return -1;
        }
        return nsent;
    }
};
