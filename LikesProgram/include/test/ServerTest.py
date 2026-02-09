import socket
import threading
import time
import random

HOST, PORT = "127.0.0.1", 8080
MAX_INFLIGHT = 16

WELCOME_TIMEOUT = 1.0
READ_TIMEOUT = 0.5
BUF_SIZE = 4096

connections = []                 # list[socket.socket|None]
conn_names = []                  # list[str]
conn_lock = threading.Lock()
print_lock = threading.Lock()
stop_event = threading.Event()

# ---- stress control ----
stress_thread = None
stress_stop = threading.Event()
# ---- reconnect control ----
reconnect_thread = None
reconnect_stop_evt = threading.Event()

def safe_print(*args, **kwargs):
    with print_lock:
        print(*args, **kwargs)

def parse_welcome_name(data: bytes) -> str:
    text = data.decode("utf-8", errors="replace").strip()
    if text.startswith("NAME:"):
        return text[5:].strip()
    if text.lower().startswith("welcome "):
        return text.split(" ", 1)[1].strip()
    return text

def set_name(idx: int, name: str):
    with conn_lock:
        conn_names[idx - 1] = name

def get_name(idx: int) -> str:
    with conn_lock:
        name = conn_names[idx - 1]
    return name if name else f"#{idx}"

def connect_one(idx):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        s.settimeout(WELCOME_TIMEOUT)

        try:
            data = s.recv(BUF_SIZE)
            if data:
                name = parse_welcome_name(data)
                set_name(idx, name)
                safe_print(f"[{idx} {get_name(idx)}] welcome: {data!r}")
            else:
                set_name(idx, "")
        except socket.timeout:
            set_name(idx, "")
        except OSError as e:
            safe_print(f"[{idx}] <welcome recv error> {e}")
            set_name(idx, "")

        s.settimeout(READ_TIMEOUT)

        with conn_lock:
            connections[idx - 1] = s

        t = threading.Thread(target=reader_loop, args=(idx,), daemon=True)
        t.start()

    except Exception as e:
        safe_print(f"[{idx}] connect failed: {e!r}")
        with conn_lock:
            connections[idx - 1] = None
            conn_names[idx - 1] = ""

def reader_loop(idx):
    while not stop_event.is_set():
        with conn_lock:
            s = connections[idx - 1] if 1 <= idx <= len(connections) else None
        if s is None:
            return

        try:
            data = s.recv(BUF_SIZE)
            if not data:
                safe_print(f"[{idx} {get_name(idx)}] <closed by peer>")
                with conn_lock:
                    try: s.close()
                    except Exception: pass
                    connections[idx - 1] = None
                return

            safe_print(f"[{idx} {get_name(idx)}] recv: {data!r}")

        except socket.timeout:
            continue
        except OSError as e:
            safe_print(f"[{idx} {get_name(idx)}] <recv error> {e}")
            with conn_lock:
                try: s.close()
                except Exception: pass
                connections[idx - 1] = None
            return

def send_on(idx, payload: bytes, *, quiet=False):
    with conn_lock:
        s = connections[idx - 1] if 1 <= idx <= len(connections) else None
        name = conn_names[idx - 1] if 1 <= idx <= len(conn_names) else ""
    if s is None:
        if not quiet:
            safe_print(f"[{idx} {name or f'#{idx}'}] <not connected>")
        return False

    try:
        s.sendall(payload)
        if not quiet:
            safe_print(f"[{idx} {name or f'#{idx}'}] sent: {payload!r}")
        return True
    except OSError as e:
        if not quiet:
            safe_print(f"[{idx} {name or f'#{idx}'}] <send error> {e}")
        with conn_lock:
            try: s.close()
            except Exception: pass
            connections[idx - 1] = None
        return False

def close_all():
    stop_event.set()
    reconnect_stop_evt.set()
    stress_stop.set()
    with conn_lock:
        for i, s in enumerate(connections, start=1):
            if s:
                try: s.close()
                except Exception: pass
            connections[i - 1] = None
            conn_names[i - 1] = ""

# ---------------- reconnect helpers -------------
def graceful_disconnect_one(idx: int, quit_cmd: bytes = b"quit"):
    """
    优雅断开：
      1. 尝试发送 quit 给服务器
      2. 再关闭 socket
    """
    with conn_lock:
        s = connections[idx - 1] if 1 <= idx <= len(connections) else None
        # 先从表里摘掉，避免并发 send/recv 再用到
        connections[idx - 1] = None
        # 名字是否清空按你需求，这里保留
        name = conn_names[idx - 1]

    if s:
        try:
            # 尝试发送 quit（短暂超时，避免卡死）
            s.settimeout(0.2)
            s.sendall(quit_cmd)
            safe_print(f"[{idx} {name or f'#{idx}'}] sent quit")
        except Exception as e:
            safe_print(f"[{idx} {name or f'#{idx}'}] quit send failed: {e}")
        finally:
            try:
                s.close()
            except Exception:
                pass

def reconnect_one(idx: int):
    """断开后立刻重连（复用你现有 connect_one）。"""
    graceful_disconnect_one(idx, quit_cmd=b"QUIT\r\n")
    connect_one(idx)

def start_reconnect(seconds: float, rate: float, mode: str = "rr"):
    """
    连续断开重连压测：
      - seconds: 持续时间
      - rate: 每秒断开重连次数（总次数，分布到 MAX_INFLIGHT 个连接上）
      - mode: rr(轮询) / rand(随机)
    """
    global reconnect_thread
    if reconnect_thread is not None and reconnect_thread.is_alive():
        safe_print("[reconnect] already running. use 'reconnect_stop' first.")
        return

    reconnect_stop_evt.clear()

    def run():
        interval = 1.0 / rate if rate > 0 else 0.0
        end_at = time.monotonic() + seconds

        ok = 0
        fail = 0
        t0 = time.monotonic()
        next_at = t0
        rr = 1
        last_report = t0

        safe_print(f"[reconnect] start: seconds={seconds}, rate={rate}/s, mode={mode}, conns={MAX_INFLIGHT}")

        while not reconnect_stop_evt.is_set() and time.monotonic() < end_at:
            now = time.monotonic()
            if interval > 0 and now < next_at:
                time.sleep(min(0.01, next_at - now))
                continue
            next_at += interval

            if mode == "rand":
                idx = random.randint(1, MAX_INFLIGHT)
            else:
                idx = rr
                rr += 1
                if rr > MAX_INFLIGHT:
                    rr = 1

            try:
                reconnect_one(idx)
                ok += 1
            except Exception as e:
                fail += 1
                safe_print(f"[reconnect] idx={idx} error: {e!r}")

            if now - last_report >= 1.0:
                elapsed = now - t0
                cur_rate = ok / elapsed if elapsed > 0 else 0.0
                safe_print(f"[reconnect] ok={ok} fail={fail} rate~{cur_rate:.1f}/s")
                last_report = now

        elapsed = time.monotonic() - t0
        cur_rate = ok / elapsed if elapsed > 0 else 0.0
        safe_print(f"[reconnect] done: ok={ok} fail={fail} elapsed={elapsed:.2f}s rate~{cur_rate:.1f}/s")

    reconnect_thread = threading.Thread(target=run, daemon=True)
    reconnect_thread.start()

def stop_reconnect():
    reconnect_stop_evt.set()
    safe_print("[reconnect] stop requested")

# ---------------- stress helpers ----------------

def snapshot_ok_indices():
    with conn_lock:
        return [i + 1 for i, s in enumerate(connections) if s is not None]

def start_stress(seconds: float, qps: float, payload: bytes):
    """
    后台压测：按总 qps 发送，轮询所有 OK 连接。
    """
    global stress_thread
    if stress_thread is not None and stress_thread.is_alive():
        safe_print("[stress] already running. use 'stress_stop' first.")
        return

    stress_stop.clear()

    def run():
        ok_ids = snapshot_ok_indices()
        if not ok_ids:
            safe_print("[stress] no connections available.")
            return

        interval = 1.0 / qps if qps > 0 else 0.0
        end_at = time.monotonic() + seconds

        sent = 0
        failed = 0
        t0 = time.monotonic()
        next_at = t0

        rr = 0
        last_report = t0

        safe_print(f"[stress] start: seconds={seconds}, qps={qps}, conns={len(ok_ids)}, payload_len={len(payload)}")

        while not stress_stop.is_set() and time.monotonic() < end_at:
            now = time.monotonic()
            if interval > 0 and now < next_at:
                time.sleep(min(0.005, next_at - now))
                continue
            next_at += interval

            ok_ids = snapshot_ok_indices()
            if not ok_ids:
                failed += 1
                continue

            rr %= len(ok_ids)
            idx = ok_ids[rr]
            rr += 1

            if send_on(idx, payload, quiet=True):
                sent += 1
            else:
                failed += 1

            # 每秒报一次统计
            if now - last_report >= 1.0:
                elapsed = now - t0
                cur_qps = sent / elapsed if elapsed > 0 else 0.0
                safe_print(f"[stress] sent={sent} failed={failed} qps~{cur_qps:.1f} ok_conns={len(ok_ids)}")
                last_report = now

        elapsed = time.monotonic() - t0
        cur_qps = sent / elapsed if elapsed > 0 else 0.0
        safe_print(f"[stress] done: sent={sent} failed={failed} elapsed={elapsed:.2f}s qps~{cur_qps:.1f}")

    stress_thread = threading.Thread(target=run, daemon=True)
    stress_thread.start()

def stop_stress():
    stress_stop.set()
    safe_print("[stress] stop requested")

# ---------------- main ----------------

def main():
    global connections, conn_names
    connections = [None] * MAX_INFLIGHT
    conn_names = [""] * MAX_INFLIGHT

    ts = []
    for i in range(1, MAX_INFLIGHT + 1):
        t = threading.Thread(target=connect_one, args=(i,), daemon=True)
        t.start()
        ts.append(t)

    for t in ts:
        t.join()

    ok = sum(1 for s in connections if s is not None)
    safe_print(f"\nConnected: {ok}/{MAX_INFLIGHT}")
    safe_print("Commands:")
    safe_print("  <id>:<text>                send on a connection")
    safe_print("  list                       list connections")
    safe_print("  stress:<sec>:<qps>[:text]  start stress test (background)")
    safe_print("  stress_stop                stop stress test")
    safe_print("  reconnect:<sec>:<rate>[:rr|rand]  disconnect+reconnect loop (background)")
    safe_print("  reconnect_stop                  stop reconnect test")
    safe_print("  quit/exit                  exit\n")

    while True:
        try:
            line = input("cmd> ").rstrip("\n")
        except (EOFError, KeyboardInterrupt):
            safe_print("\nexit")
            break

        if not line:
            continue

        cmd = line.strip().lower()
        if cmd in ("quit", "exit"):
            break

        if cmd == "list":
            with conn_lock:
                for i, s in enumerate(connections, start=1):
                    name = conn_names[i - 1] or f"#{i}"
                    safe_print(f"{i}: {'OK' if s else 'DOWN'} name={name}")
            continue

        if cmd == "reconnect_stop":
            stop_reconnect()
            continue
        
        if cmd.startswith("reconnect:"):
            parts = line.split(":", 3)
            if len(parts) < 3:
                safe_print("usage: reconnect:<seconds>:<rate>[:rr|rand]")
                continue
            try:
                seconds = float(parts[1])
                rate = float(parts[2])
            except ValueError:
                safe_print("reconnect seconds/rate must be numbers. e.g., reconnect:30:5:rand")
                continue
            
            mode = parts[3].strip().lower() if len(parts) == 4 else "rr"
            if mode not in ("rr", "rand"):
                safe_print("mode must be rr or rand. e.g., reconnect:30:5:rr")
                continue
            
            start_reconnect(seconds, rate, mode)
            continue

        if cmd == "stress_stop":
            stop_stress()
            continue

        # stress:<sec>:<qps>[:payload]
        if cmd.startswith("stress:"):
            parts = line.split(":", 3)
            if len(parts) < 3:
                safe_print("usage: stress:<seconds>:<qps>[:payload]")
                continue
            try:
                seconds = float(parts[1])
                qps = float(parts[2])
            except ValueError:
                safe_print("stress seconds/qps must be numbers. e.g., stress:10:200:ping")
                continue

            text = parts[3] if len(parts) == 4 else "ping"
            payload = text.encode("utf-8")
            # 行协议就加：payload += b"\r\n"
            start_stress(seconds, qps, payload)
            continue

        if ":" not in line:
            safe_print("format: <conn_id>:<text>  (e.g., 1:hello)")
            continue

        left, right = line.split(":", 1)
        left = left.strip()
        if not left.isdigit():
            safe_print("conn_id must be an integer, e.g., 1:hello")
            continue

        idx = int(left)
        if idx < 1 or idx > MAX_INFLIGHT:
            safe_print(f"conn_id out of range: 1..{MAX_INFLIGHT}")
            continue

        payload = right.encode("utf-8")
        # 行协议就加：payload += b"\r\n"
        send_on(idx, payload)

    close_all()
    safe_print("closed all")

if __name__ == "__main__":
    main()
