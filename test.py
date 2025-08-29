import asyncio, time

async def worker(host, port, count):
    reader, writer = await asyncio.open_connection(host, port)
    for i in range(count):
        msg = f"hello-{i}\n".encode()
        writer.write(msg)
        await writer.drain()
        data = await reader.readexactly(len(msg))
        if data != msg:
            print("mismatch!")
    writer.close()
    await writer.wait_closed()

async def main():
    N_CONN = 1000   # 并发连接数
    N_MSG  = 10000  # 每个连接发多少消息
    start = time.time()
    await asyncio.gather(*(worker("127.0.0.1", 8080, N_MSG) for _ in range(N_CONN)))
    print("elapsed:", time.time() - start)

asyncio.run(main())
