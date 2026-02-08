import socket, threading

def recv_some(s, tag, timeout=2.0):
    s.settimeout(timeout)
    try:
        data = s.recv(4096)
        print(f"{tag}: {data!r}")
        return data
    except socket.timeout:
        print(f"{tag}: <timeout>")
        return b""

def work(i):
    msg = f"msg-{i}".encode()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect(("127.0.0.1", 8080))

        # 1) 先读欢迎消息（可能为空/可能和 echo 合并）
        first = recv_some(s, f"[{i}] welcome")

        # 2) 发送
        s.sendall(msg)

        # 3) 读回显：如果 first 里已经包含 msg，就不再强求第二次读
        if msg in first:
            print(f"[{i}] echo: <already in welcome packet>")
        else:
            recv_some(s, f"[{i}] echo")

ts = []
for i in range(3000):
    t = threading.Thread(target=work, args=(i,))
    t.start()
    ts.append(t)
for t in ts:
    t.join()
