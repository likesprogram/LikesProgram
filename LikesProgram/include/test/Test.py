import psutil
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import time
from collections import deque

# 单位换算
def format_bytes(size):
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if size < 1024:
            return f"{size:.2f} {unit}"
        size /= 1024
    return f"{size:.2f} PB"

PROC_NAME = "LikesProgram.exe"

def find_process(name):
    for proc in psutil.process_iter(['pid', 'name']):
        if proc.info['name'] and proc.info['name'].lower() == name.lower():
            return proc
    return None

proc = find_process(PROC_NAME)
if not proc:
    print(f"目标进程 {PROC_NAME} 未找到！")
    exit(1)

# 队列
MAX_SEC = 1000
MAX_MIN = 1000
MAX_HOUR = 10000

cpu_sec, mem_sec, time_sec = deque(maxlen=MAX_SEC), deque(maxlen=MAX_SEC), deque(maxlen=MAX_SEC)
cpu_min, mem_min, time_min = deque(maxlen=MAX_MIN), deque(maxlen=MAX_MIN), deque(maxlen=MAX_MIN)
cpu_hour, mem_hour, time_hour = deque(maxlen=MAX_HOUR), deque(maxlen=MAX_HOUR), deque(maxlen=MAX_HOUR)

# 累积用于分钟/小时平均
cpu_acc_min, mem_acc_min, count_min = 0, 0, 0
cpu_acc_hour, mem_acc_hour, count_hour = 0, 0, 0

start_time = time.time()

fig, (ax_sec, ax_min, ax_hour) = plt.subplots(3, 1, figsize=(12, 10))
ax_mem_sec, ax_mem_min, ax_mem_hour = ax_sec.twinx(), ax_min.twinx(), ax_hour.twinx()

# 绘制 Memory 函数，使用统一横轴
def plot_memory(ax_mem, mem_data, time_data):
    if not mem_data:
        return
    max_mem = max(mem_data)
    units = ['B', 'KB', 'MB', 'GB', 'TB']
    scale_index = 0
    while max_mem >= 1024 and scale_index < len(units) - 1:
        max_mem /= 1024
        scale_index += 1
    scale_factor = 1024 ** scale_index
    display_unit = units[scale_index]
    mem_scaled = [m / scale_factor for m in mem_data]
    ax_mem.plot(time_data, mem_scaled, color="orange")
    ax_mem.set_ylabel(f"Memory ({display_unit})")
    ax_mem.legend([f"Memory ({display_unit})"], loc="upper right")
last_minute_time = start_time
last_hour_time = start_time

def update(frame):
    global cpu_acc_min, mem_acc_min, count_min, last_minute_time
    global cpu_acc_hour, mem_acc_hour, count_hour, last_hour_time

    now = time.time()
    elapsed_sec = now - start_time

    try:
        cpu = proc.cpu_percent(interval=None)
        mem = proc.memory_info().rss
    except psutil.NoSuchProcess:
        print("进程已退出")
        exit(0)

    # --- 秒图，每秒采样 ---
    cpu_sec.append(cpu)
    mem_sec.append(mem)
    time_sec.append(elapsed_sec)

    ax_sec.clear()
    ax_sec.plot(time_sec, cpu_sec, label="CPU %", color="blue")
    ax_sec.set_ylabel("CPU %")
    ax_sec.set_ylim(0, 100)
    ax_sec.set_xlabel("Time (s)")
    ax_sec.legend(loc="upper left")
    ax_mem_sec.clear()
    plot_memory(ax_mem_sec, mem_sec, time_sec)

    # --- 累积用于分钟 ---
    cpu_acc_min += cpu
    mem_acc_min += mem
    count_min += 1
    if now - last_minute_time >= 60:  # 每分钟更新一次
        avg_cpu = cpu_acc_min / count_min
        avg_mem = mem_acc_min / count_min
        cpu_min.append(avg_cpu)
        mem_min.append(avg_mem)
        time_min.append(elapsed_sec / 60)
        cpu_acc_min, mem_acc_min, count_min = 0, 0, 0
        last_minute_time = now

    ax_min.clear()
    ax_min.plot(time_min, cpu_min, label="CPU %", color="blue")
    ax_min.set_ylabel("CPU %")
    ax_min.set_ylim(0, 100)
    ax_min.set_xlabel("Time (min)")
    ax_min.legend(loc="upper left")
    ax_mem_min.clear()
    plot_memory(ax_mem_min, mem_min, time_min)

    # --- 累积用于小时 ---
    cpu_acc_hour += cpu
    mem_acc_hour += mem
    count_hour += 1
    if now - last_hour_time >= 3600:  # 每小时更新一次
        avg_cpu = cpu_acc_hour / count_hour
        avg_mem = mem_acc_hour / count_hour
        cpu_hour.append(avg_cpu)
        mem_hour.append(avg_mem)
        time_hour.append(elapsed_sec / 3600)
        cpu_acc_hour, mem_acc_hour, count_hour = 0, 0, 0
        last_hour_time = now

    ax_hour.clear()
    ax_hour.plot(time_hour, cpu_hour, label="CPU %", color="blue")
    ax_hour.set_ylabel("CPU %")
    ax_hour.set_ylim(0, 100)
    ax_hour.set_xlabel("Time (h)")
    ax_hour.legend(loc="upper left")
    ax_mem_hour.clear()
    plot_memory(ax_mem_hour, mem_hour, time_hour)

ani = animation.FuncAnimation(fig, update, interval=1000)
plt.tight_layout()
plt.show()
