#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${1:-$(pwd)}"
BUILD_DIR="${2:-${ROOT_DIR}/build-linux-release}"
OUT_ROOT="${3:-${ROOT_DIR}/docs/progress/performance}"

REQUESTS="${LP_HTTP_BENCH_REQUESTS:-8000}"
WARMUP_REQUESTS="${LP_HTTP_BENCH_WARMUP_REQUESTS:-1000}"
CONCURRENCY_POINTS="${LP_HTTP_BENCH_CONCURRENCY:-1 2 4 8 16 32 64 128 256 512 1024 2048}"
BENCH_TOOL="${LP_HTTP_BENCH_TOOL:-auto}"
DURATION_SECONDS="${LP_HTTP_BENCH_DURATION_SECONDS:-30}"
SEGMENT_SECONDS="${LP_HTTP_BENCH_SEGMENT_SECONDS:-0}"
WARMUP_SECONDS="${LP_HTTP_BENCH_WARMUP_SECONDS:-3}"
WRK_THREADS="${LP_HTTP_BENCH_WRK_THREADS:-$(nproc 2>/dev/null || echo 4)}"
LP_PORT="${LP_HTTP_BENCH_LP_PORT:-18080}"
NGINX_PORT="${LP_HTTP_BENCH_NGINX_PORT:-18081}"
LP_WORKERS="${LP_HTTP_BENCH_WORKERS:-}"
NGINX_WORKERS="${LP_HTTP_BENCH_NGINX_WORKERS:-auto}"
LP_SERVER_MODE="${LP_HTTP_BENCH_SERVER_MODE:-standard}"
LP_BIND_ADDRESS="${LP_HTTP_BENCH_BIND_ADDRESS:-127.0.0.1}"
FIXED_QPS_POINTS="${LP_HTTP_FIXED_QPS_POINTS:-5000 25000 75000}"
FIXED_QPS_DURATION_SECONDS="${LP_HTTP_FIXED_QPS_DURATION_SECONDS:-30}"
FIXED_QPS_WORKERS="${LP_HTTP_FIXED_QPS_WORKERS:-256}"
FIXED_QPS_ENABLE="${LP_HTTP_FIXED_QPS_ENABLE:-1}"
FIXED_QPS_ENGINE="${LP_HTTP_FIXED_QPS_ENGINE:-auto}"
KEEP_RESULT_RUNS="${LP_HTTP_BENCH_KEEP_RUNS:-0}"
SERVER_CPUSET="${LP_HTTP_BENCH_SERVER_CPUSET:-}"
CLIENT_CPUSET="${LP_HTTP_BENCH_CLIENT_CPUSET:-}"
RESOURCE_MINIMUM_SAMPLE_PERCENT="${LP_HTTP_RESOURCE_MINIMUM_SAMPLE_PERCENT:-90}"
TEST_TYPE="${LP_HTTP_TEST_TYPE:-http_nginx_matrix}"
TEST_PLATFORM="${LP_HTTP_TEST_PLATFORM:-$(uname -s)-$(uname -r)}"
TEST_BACKEND="${LP_HTTP_TEST_BACKEND:-${LIKESPROGRAM_NET_POLLER_BACKEND:-auto}}"

STAMP="$(date +%Y%m%d-%H%M%S)"
STARTED_AT="$(date --iso-8601=seconds)"
START_EPOCH="$(date +%s)"
RESULT_DIR="${OUT_ROOT}/net-nginx-http-${STAMP}"
LP_SERVER="${BUILD_DIR}/packages/LikesProgramNet/HttpBenchmarkServer"
LP_LIB_PATH="${BUILD_DIR}/packages/LikesProgramCore:${BUILD_DIR}/packages/LikesProgramNet"
CSV_FILE="${RESULT_DIR}/qps-latency.csv"
SEGMENT_CSV_FILE="${RESULT_DIR}/qps-latency-segments.csv"
SVG_FILE="${RESULT_DIR}/qps-latency.svg"
THROUGHPUT_SVG="${RESULT_DIR}/concurrency-qps.svg"
RESOURCE_FILE="${RESULT_DIR}/likesprogram-resource.csv"
RESOURCE_SUMMARY="${RESULT_DIR}/likesprogram-resource-summary.csv"
RESOURCE_SVG="${RESULT_DIR}/likesprogram-resource.svg"
RESOURCE_COUNTS_SVG="${RESULT_DIR}/likesprogram-resource-counts.svg"
RESOURCE_STOP="${RESULT_DIR}/likesprogram-resource.stop"
FIXED_QPS_TOOL="${ROOT_DIR}/tools/net-fixed-qps-timeseries.py"
RESULT_ROTATION_TOOL="${ROOT_DIR}/tools/performance-result-rotation.py"
TEST_RECORD_TOOL="${ROOT_DIR}/tools/performance-test-records.py"
WRK_RESULT_TOOL="${ROOT_DIR}/tools/net-wrk-result.py"
NGINX_PREFIX="${RESULT_DIR}/nginx"
REPORT_HTML="${RESULT_DIR}/index.html"
LP_PID=""
MONITOR_PID=""
ACTUAL_TOOL=""
TEST_OUTCOME="passed"
RUN_FINISHED=0
COMPLETED_STAGE_COUNT=0
PLANNED_STAGE_COUNT=0
PLANNED_DURATION_SECONDS=0
VERSION="${LP_HTTP_TEST_VERSION:-$(git -C "${ROOT_DIR}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}"

if [[ ! "${DURATION_SECONDS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "LP_HTTP_BENCH_DURATION_SECONDS must be a positive integer" >&2
    exit 1
fi
if [[ ! "${SEGMENT_SECONDS}" =~ ^[0-9]+$ ]]; then
    echo "LP_HTTP_BENCH_SEGMENT_SECONDS must be a non-negative integer" >&2
    exit 1
fi

# 计划阶段按每个服务的独立测量点计数，业务失败不减少完成数。
CONCURRENCY_STAGE_COUNT=0
for _ in ${CONCURRENCY_POINTS}; do
    ((CONCURRENCY_STAGE_COUNT += 1))
done
PLANNED_STAGE_COUNT=$((CONCURRENCY_STAGE_COUNT * 2))
FIXED_QPS_STAGE_COUNT=0
if [[ "${FIXED_QPS_ENABLE}" != "0" ]]; then
    for _ in ${FIXED_QPS_POINTS}; do
        ((FIXED_QPS_STAGE_COUNT += 2))
    done
    PLANNED_STAGE_COUNT=$((PLANNED_STAGE_COUNT + FIXED_QPS_STAGE_COUNT))
fi

mkdir -p "${RESULT_DIR}" "${NGINX_PREFIX}/logs" "${NGINX_PREFIX}/conf"
python3 "${RESULT_ROTATION_TOOL}" --out-root "${OUT_ROOT}" --keep-runs "${KEEP_RESULT_RUNS}"
ulimit -n 1048576 >/dev/null 2>&1 || ulimit -n 65535 >/dev/null 2>&1 || true

resolve_bench_tool() {
    if [[ "${BENCH_TOOL}" == "auto" ]]; then
        if command -v wrk >/dev/null 2>&1; then
            echo "wrk"
        else
            echo "ab"
        fi
        return
    fi

    echo "${BENCH_TOOL}"
}

run_client() {
    if [[ -n "${CLIENT_CPUSET}" ]]; then
        taskset -c "${CLIENT_CPUSET}" "$@"
        return
    fi

    "$@"
}

parse_wrk_result() {
    local log_file="$1"
    python3 "${WRK_RESULT_TOOL}" "${log_file}"
}

monitor_likesprogram_resource() {
    local pid="$1"
    local out_file="$2"
    local stop_file="$3"
    local hz
    local page_kib

    hz="$(getconf CLK_TCK)"
    page_kib="$(($(getconf PAGESIZE) / 1024))"

    echo "timestamp,cpu_percent,rss_kib,vsz_kib,thread_count,fd_count" > "${out_file}"
    if [[ ! -r "/proc/${pid}/stat" || ! -r "/proc/${pid}/statm" ]]; then
        return 0
    fi
    local prev_ticks
    local prev_ns
    prev_ticks="$(awk '{ print $14 + $15 }' "/proc/${pid}/stat")"
    prev_ns="$(date +%s%N)"

    while kill -0 "${pid}" >/dev/null 2>&1 && [[ ! -f "${stop_file}" ]]; do
        sleep 1
        [[ -r "/proc/${pid}/stat" && -r "/proc/${pid}/statm" ]] || break

        local now_ticks
        local now_ns
        local delta_ticks
        local delta_ns
        local cpu_percent
        local size_pages
        local resident_pages
        local rss_kib
        local vsz_kib
        local thread_count
        local fd_count

        now_ticks="$(awk '{ print $14 + $15 }' "/proc/${pid}/stat")"
        now_ns="$(date +%s%N)"
        delta_ticks=$((now_ticks - prev_ticks))
        delta_ns=$((now_ns - prev_ns))
        cpu_percent="$(awk -v ticks="${delta_ticks}" -v hz="${hz}" -v ns="${delta_ns}" 'BEGIN {
            if (ns <= 0) {
                printf "0.00";
            } else {
                printf "%.2f", (ticks / hz) / (ns / 1000000000.0) * 100.0;
            }
        }')"

        read -r size_pages resident_pages _ < "/proc/${pid}/statm"
        rss_kib=$((resident_pages * page_kib))
        vsz_kib=$((size_pages * page_kib))
        thread_count="$(find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)"
        fd_count="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"

        echo "$(date -Is),${cpu_percent},${rss_kib},${vsz_kib},${thread_count},${fd_count}" >> "${out_file}"
        prev_ticks="${now_ticks}"
        prev_ns="${now_ns}"
    done
}

write_resource_summary() {
    python3 - "${RESOURCE_FILE}" "${RESOURCE_SUMMARY}" <<'PY'
import csv
import sys

source = sys.argv[1]
target = sys.argv[2]

rows = []
with open(source, "r", encoding="utf-8", newline="") as handle:
    reader = csv.DictReader(handle)
    for row in reader:
        rows.append({
            "cpu": float(row["cpu_percent"]),
            "rss": float(row["rss_kib"]) / 1024.0,
            "vsz": float(row["vsz_kib"]) / 1024.0,
            "threads": int(row.get("thread_count") or 0),
            "fds": int(row.get("fd_count") or 0),
        })

def average(values):
    return sum(values) / len(values) if values else 0.0

summary = {
    "samples": len(rows),
    "avg_cpu_percent": average([row["cpu"] for row in rows]),
    "max_cpu_percent": max([row["cpu"] for row in rows], default=0.0),
    "avg_rss_mib": average([row["rss"] for row in rows]),
    "max_rss_mib": max([row["rss"] for row in rows], default=0.0),
    "avg_vsz_mib": average([row["vsz"] for row in rows]),
    "max_vsz_mib": max([row["vsz"] for row in rows], default=0.0),
    "avg_thread_count": average([row["threads"] for row in rows]),
    "max_thread_count": max([row["threads"] for row in rows], default=0),
    "avg_fd_count": average([row["fds"] for row in rows]),
    "max_fd_count": max([row["fds"] for row in rows], default=0),
}

with open(target, "w", encoding="utf-8", newline="") as handle:
    writer = csv.writer(handle)
    writer.writerow(["metric", "value"])
    for key, value in summary.items():
        if key == "samples":
            writer.writerow([key, str(value)])
        else:
            writer.writerow([key, f"{value:.2f}"])
PY
}

write_resource_svg() {
    python3 - "${RESOURCE_FILE}" "${RESOURCE_SVG}" "${STAMP}" <<'PY'
import csv
import sys
from pathlib import Path

csv_path = Path(sys.argv[1])
svg_path = Path(sys.argv[2])
stamp = sys.argv[3]

rows = []
with csv_path.open("r", encoding="utf-8", newline="") as handle:
    reader = csv.DictReader(handle)
    for index, row in enumerate(reader):
        rows.append({
            "second": index,
            "cpu": float(row["cpu_percent"]),
            "rss_mib": float(row["rss_kib"]) / 1024.0,
        })

width = 960
height = 560
left = 86
right = 68
top = 54
bottom = 72
plot_w = width - left - right
plot_h = height - top - bottom
max_second = max((row["second"] for row in rows), default=1)
max_cpu = max((row["cpu"] for row in rows), default=1.0)
max_rss = max((row["rss_mib"] for row in rows), default=1.0)
left_max = max(1.0, max_cpu * 1.15)
right_max = max(1.0, max_rss * 1.15)

def x_pos(second):
    return left + (second / max(1.0, max_second)) * plot_w

def y_left(value):
    return top + plot_h - (value / left_max) * plot_h

def y_right(value):
    return top + plot_h - (value / right_max) * plot_h

parts = [
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
    '<rect width="100%" height="100%" fill="#ffffff"/>',
    f'<text x="{left}" y="30" font-family="Arial, sans-serif" font-size="22" font-weight="700" fill="#111827">LikesProgramNet resource usage vs duration</text>',
    f'<text x="{left}" y="50" font-family="Arial, sans-serif" font-size="12" fill="#4b5563">run={stamp}, x=duration(s), left=CPU(%), right=RSS(MiB)</text>',
]

for i in range(6):
    value = left_max * i / 5
    y = y_left(value)
    parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e5e7eb" stroke-width="1"/>')
    parts.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="Arial, sans-serif" font-size="12" fill="#0072B2">{value:.0f}</text>')
    rss_value = right_max * i / 5
    parts.append(f'<text x="{left + plot_w + 10}" y="{y + 4:.2f}" text-anchor="start" font-family="Arial, sans-serif" font-size="12" fill="#D55E00">{rss_value:.1f}</text>')

for i in range(6):
    value = max_second * i / 5
    x = x_pos(value)
    parts.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f3f4f6" stroke-width="1"/>')
    parts.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="Arial, sans-serif" font-size="12" fill="#374151">{value:.0f}</text>')

parts.append(f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#0072B2" stroke-width="1.4"/>')
parts.append(f'<line x1="{left + plot_w}" y1="{top}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#D55E00" stroke-width="1.4"/>')
parts.append(f'<text x="{left + plot_w / 2}" y="{height - 20}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827">Duration (s)</text>')
parts.append(f'<text x="22" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#0072B2" transform="rotate(-90 22 {top + plot_h / 2})">CPU (%)</text>')
parts.append(f'<text x="{width - 20}" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#D55E00" transform="rotate(90 {width - 20} {top + plot_h / 2})">RSS (MiB)</text>')

if rows:
    cpu_points = " ".join(f'{x_pos(row["second"]):.2f},{y_left(row["cpu"]):.2f}' for row in rows)
    rss_points = " ".join(f'{x_pos(row["second"]):.2f},{y_right(row["rss_mib"]):.2f}' for row in rows)
    parts.append(f'<polyline points="{cpu_points}" fill="none" stroke="#0072B2" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')
    parts.append(f'<polyline points="{rss_points}" fill="none" stroke="#D55E00" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')

legend_x = left + 18
legend_y = top + 26
parts.append(f'<rect x="{legend_x - 12}" y="{legend_y - 22}" width="164" height="58" fill="#ffffff" stroke="#d1d5db" rx="4"/>')
parts.append(f'<line x1="{legend_x}" y1="{legend_y}" x2="{legend_x + 34}" y2="{legend_y}" stroke="#0072B2" stroke-width="3"/>')
parts.append(f'<text x="{legend_x + 44}" y="{legend_y + 4}" font-family="Arial, sans-serif" font-size="13" fill="#111827">CPU</text>')
parts.append(f'<line x1="{legend_x}" y1="{legend_y + 24}" x2="{legend_x + 34}" y2="{legend_y + 24}" stroke="#D55E00" stroke-width="3"/>')
parts.append(f'<text x="{legend_x + 44}" y="{legend_y + 28}" font-family="Arial, sans-serif" font-size="13" fill="#111827">RSS</text>')
parts.append('</svg>')
svg_path.write_text("\n".join(parts), encoding="utf-8")
PY
}

write_resource_counts_svg() {
    python3 - "${RESOURCE_FILE}" "${RESOURCE_COUNTS_SVG}" "${STAMP}" <<'PY'
import csv
import sys
from pathlib import Path

csv_path = Path(sys.argv[1])
svg_path = Path(sys.argv[2])
stamp = sys.argv[3]

rows = []
with csv_path.open("r", encoding="utf-8", newline="") as handle:
    reader = csv.DictReader(handle)
    for index, row in enumerate(reader):
        rows.append({
            "second": index,
            "threads": float(row.get("thread_count") or 0.0),
            "fds": float(row.get("fd_count") or 0.0),
        })

width = 960
height = 560
left = 86
right = 68
top = 54
bottom = 72
plot_w = width - left - right
plot_h = height - top - bottom
max_second = max((row["second"] for row in rows), default=1)
max_threads = max((row["threads"] for row in rows), default=1.0)
max_fds = max((row["fds"] for row in rows), default=1.0)
left_max = max(1.0, max_threads * 1.15)
right_max = max(1.0, max_fds * 1.15)

def x_pos(second):
    return left + (second / max(1.0, max_second)) * plot_w

def y_left(value):
    return top + plot_h - (value / left_max) * plot_h

def y_right(value):
    return top + plot_h - (value / right_max) * plot_h

parts = [
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
    '<rect width="100%" height="100%" fill="#ffffff"/>',
    f'<text x="{left}" y="30" font-family="Arial, sans-serif" font-size="22" font-weight="700" fill="#111827">LikesProgramNet thread/FD counts vs duration</text>',
    f'<text x="{left}" y="50" font-family="Arial, sans-serif" font-size="12" fill="#4b5563">run={stamp}, x=duration(s), left=threads, right=FDs</text>',
]

for i in range(6):
    value = left_max * i / 5
    y = y_left(value)
    parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e5e7eb" stroke-width="1"/>')
    parts.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="Arial, sans-serif" font-size="12" fill="#009E73">{value:.0f}</text>')
    fd_value = right_max * i / 5
    parts.append(f'<text x="{left + plot_w + 10}" y="{y + 4:.2f}" text-anchor="start" font-family="Arial, sans-serif" font-size="12" fill="#CC79A7">{fd_value:.0f}</text>')

for i in range(6):
    value = max_second * i / 5
    x = x_pos(value)
    parts.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f3f4f6" stroke-width="1"/>')
    parts.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="Arial, sans-serif" font-size="12" fill="#374151">{value:.0f}</text>')

parts.append(f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#009E73" stroke-width="1.4"/>')
parts.append(f'<line x1="{left + plot_w}" y1="{top}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#CC79A7" stroke-width="1.4"/>')
parts.append(f'<text x="{left + plot_w / 2}" y="{height - 20}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827">Duration (s)</text>')
parts.append(f'<text x="22" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#009E73" transform="rotate(-90 22 {top + plot_h / 2})">Threads</text>')
parts.append(f'<text x="{width - 20}" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#CC79A7" transform="rotate(90 {width - 20} {top + plot_h / 2})">FDs</text>')

if rows:
    thread_points = " ".join(f'{x_pos(row["second"]):.2f},{y_left(row["threads"]):.2f}' for row in rows)
    fd_points = " ".join(f'{x_pos(row["second"]):.2f},{y_right(row["fds"]):.2f}' for row in rows)
    parts.append(f'<polyline points="{thread_points}" fill="none" stroke="#009E73" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')
    parts.append(f'<polyline points="{fd_points}" fill="none" stroke="#CC79A7" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')

legend_x = left + 18
legend_y = top + 26
parts.append(f'<rect x="{legend_x - 12}" y="{legend_y - 22}" width="164" height="58" fill="#ffffff" stroke="#d1d5db" rx="4"/>')
parts.append(f'<line x1="{legend_x}" y1="{legend_y}" x2="{legend_x + 34}" y2="{legend_y}" stroke="#009E73" stroke-width="3"/>')
parts.append(f'<text x="{legend_x + 44}" y="{legend_y + 4}" font-family="Arial, sans-serif" font-size="13" fill="#111827">Threads</text>')
parts.append(f'<line x1="{legend_x}" y1="{legend_y + 24}" x2="{legend_x + 34}" y2="{legend_y + 24}" stroke="#CC79A7" stroke-width="3"/>')
parts.append(f'<text x="{legend_x + 44}" y="{legend_y + 28}" font-family="Arial, sans-serif" font-size="13" fill="#111827">FDs</text>')
parts.append('</svg>')
svg_path.write_text("\n".join(parts), encoding="utf-8")
PY
}

wait_http() {
    local url="$1"
    local name="$2"
    local expected_pid="${3:-}"
    for _ in $(seq 1 50); do
        if [[ -n "${expected_pid}" ]] && ! kill -0 "${expected_pid}" >/dev/null 2>&1; then
            echo "service exited before ready: ${name} pid=${expected_pid}" >&2
            return 1
        fi
        if run_client ab -k -n 1 -c 1 -s 2 "${url}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "service not ready: ${name}" >&2
    return 1
}

run_ab_point() {
    local server="$1"
    local url="$2"
    local concurrency="$3"
    local log_file="${RESULT_DIR}/${server}-c${concurrency}.ab.log"

    run_client ab -k -n "${WARMUP_REQUESTS}" -c "${concurrency}" -s 30 "${url}" >/dev/null
    run_client ab -k -n "${REQUESTS}" -c "${concurrency}" -s 60 "${url}" > "${log_file}"

    local failed
    failed="$(awk -F: '/Failed requests/ { gsub(/^[ \t]+/, "", $2); split($2, a, " "); print a[1] }' "${log_file}")"
    if [[ -n "${failed}" && "${failed}" != "0" ]]; then
        echo "ab failed requests for ${server} c=${concurrency}: ${failed}" >&2
        TEST_OUTCOME="failed"
    fi

    local qps
    local latency
    local p50
    local p90
    local p99
    qps="$(awk -F: '/Requests per second/ { gsub(/^[ \t]+/, "", $2); split($2, a, " "); print a[1] }' "${log_file}")"
    latency="$(awk -F: '/Time per request/ && $0 !~ /across all concurrent/ { gsub(/^[ \t]+/, "", $2); split($2, a, " "); print a[1]; exit }' "${log_file}")"
    p50="$(awk '/^[[:space:]]*50%/ { print $2; exit }' "${log_file}")"
    p90="$(awk '/^[[:space:]]*90%/ { print $2; exit }' "${log_file}")"
    p99="$(awk '/^[[:space:]]*99%/ { print $2; exit }' "${log_file}")"
    echo "${server},${concurrency},${qps},${latency},${p50:-0},${p90:-0},${p99:-0},${failed:-0},0,0,0,0,${failed:-0},ab,requests=${REQUESTS}" >> "${CSV_FILE}"
}

run_wrk_point() {
    local server="$1"
    local url="$2"
    local concurrency="$3"
    local log_file="${RESULT_DIR}/${server}-c${concurrency}.wrk.log"
    local threads

    threads="${WRK_THREADS}"
    if (( concurrency < threads )); then
        threads="${concurrency}"
    fi
    if (( threads < 1 )); then
        threads=1
    fi

    # 固定窗口只在一个服务阶段开始时预热，避免把预热流量混入 timeout 计数。
    run_client wrk -t"${threads}" -c"${concurrency}" -d"${WARMUP_SECONDS}s" --latency "${url}" > "${log_file}.warmup" 2>&1

    local segment_seconds="${DURATION_SECONDS}"
    if (( SEGMENT_SECONDS > 0 && SEGMENT_SECONDS < DURATION_SECONDS )); then
        segment_seconds="${SEGMENT_SECONDS}"
    fi

    local segment_count=$(((DURATION_SECONDS + segment_seconds - 1) / segment_seconds))
    local segment_index=1
    local elapsed_seconds=0
    while (( elapsed_seconds < DURATION_SECONDS )); do
        local duration="${segment_seconds}"
        local remaining=$((DURATION_SECONDS - elapsed_seconds))
        if (( duration > remaining )); then
            duration="${remaining}"
        fi

        local segment_log_file="${log_file}"
        if (( SEGMENT_SECONDS > 0 && SEGMENT_SECONDS < DURATION_SECONDS )); then
            segment_log_file="${RESULT_DIR}/${server}-c${concurrency}.segment-$(printf '%04d' "${segment_index}").wrk.log"
        fi

        local segment_started_at
        local segment_finished_at
        segment_started_at="$(date --iso-8601=seconds)"
        run_client wrk -t"${threads}" -c"${concurrency}" -d"${duration}s" --latency "${url}" > "${segment_log_file}" 2>&1
        segment_finished_at="$(date --iso-8601=seconds)"

        local qps
        local latency
        local p50
        local p90
        local p99
        local errors
        local connect_errors
        local read_errors
        local write_errors
        local timeout_errors
        local non_success_errors
        read -r qps latency p50 p90 p99 errors connect_errors read_errors write_errors timeout_errors non_success_errors < <(parse_wrk_result "${segment_log_file}")
        echo "${server},${concurrency},${segment_index},${segment_count},${elapsed_seconds},${duration},${segment_started_at},${segment_finished_at},${qps},${latency},${p50},${p90},${p99},${errors},${connect_errors},${read_errors},${write_errors},${timeout_errors},${non_success_errors}" >> "${SEGMENT_CSV_FILE}"
        echo "${server},${concurrency},${qps},${latency},${p50},${p90},${p99},${errors},${connect_errors},${read_errors},${write_errors},${timeout_errors},${non_success_errors},wrk,duration=${duration}s;segment=${segment_index}" >> "${CSV_FILE}"
        if [[ "${errors}" != "0" ]]; then
            echo "wrk errors for ${server} c=${concurrency} segment=${segment_index}: total=${errors} connect=${connect_errors} read=${read_errors} write=${write_errors} timeout=${timeout_errors} non_success=${non_success_errors}" >&2
            TEST_OUTCOME="failed"
        fi

        elapsed_seconds=$((elapsed_seconds + duration))
        segment_index=$((segment_index + 1))
    done
}

run_point() {
    local tool="$1"
    local server="$2"
    local url="$3"
    local concurrency="$4"

    if [[ "${tool}" == "wrk" ]]; then
        run_wrk_point "${server}" "${url}" "${concurrency}"
    else
        run_ab_point "${server}" "${url}" "${concurrency}"
    fi
}

write_svg() {
    python3 - "${CSV_FILE}" "${SVG_FILE}" "${THROUGHPUT_SVG}" "${STAMP}" <<'PY'
import csv
import sys
from pathlib import Path

csv_path = Path(sys.argv[1])
latency_svg_path = Path(sys.argv[2])
throughput_svg_path = Path(sys.argv[3])
stamp = sys.argv[4]

rows = []
with csv_path.open("r", encoding="utf-8", newline="") as handle:
    reader = csv.DictReader(handle)
    for row in reader:
        rows.append({
            "server": row["server"],
            "concurrency": int(row["concurrency"]),
            "qps": float(row["qps"]),
            "latency_ms": float(row["latency_ms"]),
            "p50_ms": float(row.get("p50_ms") or 0.0),
            "p90_ms": float(row.get("p90_ms") or 0.0),
            "p99_ms": float(row.get("p99_ms") or 0.0),
            "error_count": int(float(row.get("error_count") or 0)),
        })

series = {}
for row in rows:
    series.setdefault(row["server"], []).append(row)

width = 960
height = 600
left = 86
right = 42
top = 54
bottom = 78
plot_w = width - left - right
plot_h = height - top - bottom

def fmt(value):
    if value >= 1000:
        return f"{value:,.0f}"
    if value >= 10:
        return f"{value:.1f}"
    return f"{value:.2f}"

colors = {
    "LikesProgramNet": "#0072B2",
    "Nginx": "#D55E00",
}

def append_legend(parts):
    legend_x = width - 228
    legend_y = 66
    parts.append(f'<rect x="{legend_x - 16}" y="{legend_y - 24}" width="202" height="64" fill="#ffffff" stroke="#d1d5db" rx="4"/>')
    for index, server in enumerate(("LikesProgramNet", "Nginx")):
        y = legend_y + index * 25
        color = colors[server]
        parts.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 34}" y2="{y}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<circle cx="{legend_x + 17}" cy="{y}" r="4" fill="{color}" stroke="#ffffff" stroke-width="1.3"/>')
        parts.append(f'<text x="{legend_x + 44}" y="{y + 4}" font-family="Arial, sans-serif" font-size="14" fill="#111827">{server}</text>')

def write_latency_qps_chart(svg_path):
    for values in series.values():
        values.sort(key=lambda item: item["qps"])

    max_qps = max((row["qps"] for row in rows), default=1.0)
    max_latency = max((row["latency_ms"] for row in rows), default=1.0)
    x_max = max_qps * 1.08 if max_qps > 0 else 1.0
    y_max = max_latency * 1.15 if max_latency > 0 else 1.0

    def x_pos(qps):
        return left + (qps / x_max) * plot_w

    def y_pos(latency):
        return top + plot_h - (latency / y_max) * plot_h

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{left}" y="30" font-family="Arial, sans-serif" font-size="22" font-weight="700" fill="#111827">HTTP keep-alive response time vs QPS</text>',
        f'<text x="{left}" y="50" font-family="Arial, sans-serif" font-size="12" fill="#4b5563">run={stamp}, x=QPS, y=mean response time(ms)</text>',
    ]

    for i in range(6):
        y_value = y_max * i / 5
        y = y_pos(y_value)
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e5e7eb" stroke-width="1"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="Arial, sans-serif" font-size="12" fill="#374151">{fmt(y_value)}</text>')

    for i in range(6):
        x_value = x_max * i / 5
        x = x_pos(x_value)
        parts.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f3f4f6" stroke-width="1"/>')
        parts.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="Arial, sans-serif" font-size="12" fill="#374151">{fmt(x_value)}</text>')

    parts.append(f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
    parts.append(f'<text x="{left + plot_w / 2}" y="{height - 22}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827">QPS</text>')
    parts.append(f'<text x="22" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827" transform="rotate(-90 22 {top + plot_h / 2})">Response time (ms)</text>')

    for server in ("LikesProgramNet", "Nginx"):
        values = series.get(server, [])
        if not values:
            continue
        color = colors.get(server, "#111827")
        points = " ".join(f'{x_pos(row["qps"]):.2f},{y_pos(row["latency_ms"]):.2f}' for row in values)
        parts.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')
        for row in values:
            x = x_pos(row["qps"])
            y = y_pos(row["latency_ms"])
            label = f'c={row["concurrency"]}'
            parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4.2" fill="{color}" stroke="#ffffff" stroke-width="1.5"/>')
            parts.append(f'<text x="{x + 7:.2f}" y="{y - 7:.2f}" font-family="Arial, sans-serif" font-size="10" fill="{color}">{label}</text>')

    append_legend(parts)
    parts.append('</svg>')
    svg_path.write_text("\n".join(parts), encoding="utf-8")

def write_concurrency_qps_chart(svg_path):
    for values in series.values():
        values.sort(key=lambda item: item["concurrency"])

    max_concurrency = max((row["concurrency"] for row in rows), default=1)
    max_qps = max((row["qps"] for row in rows), default=1.0)
    x_max = max_concurrency * 1.08 if max_concurrency > 0 else 1.0
    y_max = max_qps * 1.15 if max_qps > 0 else 1.0

    def x_pos(concurrency):
        return left + (concurrency / x_max) * plot_w

    def y_pos(qps):
        return top + plot_h - (qps / y_max) * plot_h

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{left}" y="30" font-family="Arial, sans-serif" font-size="22" font-weight="700" fill="#111827">HTTP keep-alive throughput vs concurrency</text>',
        f'<text x="{left}" y="50" font-family="Arial, sans-serif" font-size="12" fill="#4b5563">run={stamp}, x=Concurrency, y=QPS, node label=mean latency(ms)</text>',
    ]

    for i in range(6):
        y_value = y_max * i / 5
        y = y_pos(y_value)
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e5e7eb" stroke-width="1"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="Arial, sans-serif" font-size="12" fill="#374151">{fmt(y_value)}</text>')

    for i in range(6):
        x_value = x_max * i / 5
        x = x_pos(x_value)
        parts.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f3f4f6" stroke-width="1"/>')
        parts.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="Arial, sans-serif" font-size="12" fill="#374151">{fmt(x_value)}</text>')

    parts.append(f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#111827" stroke-width="1.4"/>')
    parts.append(f'<text x="{left + plot_w / 2}" y="{height - 22}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827">Concurrency</text>')
    parts.append(f'<text x="22" y="{top + plot_h / 2}" text-anchor="middle" font-family="Arial, sans-serif" font-size="15" fill="#111827" transform="rotate(-90 22 {top + plot_h / 2})">Throughput (QPS)</text>')

    for server in ("LikesProgramNet", "Nginx"):
        values = series.get(server, [])
        if not values:
            continue
        color = colors.get(server, "#111827")
        points = " ".join(f'{x_pos(row["concurrency"]):.2f},{y_pos(row["qps"]):.2f}' for row in values)
        parts.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="3" stroke-linejoin="round" stroke-linecap="round"/>')
        for row in values:
            x = x_pos(row["concurrency"])
            y = y_pos(row["qps"])
            label = f'{row["latency_ms"]:.2f}ms'
            parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4.2" fill="{color}" stroke="#ffffff" stroke-width="1.5"/>')
            parts.append(f'<text x="{x + 7:.2f}" y="{y - 7:.2f}" font-family="Arial, sans-serif" font-size="10" fill="{color}">{label}</text>')

    append_legend(parts)
    parts.append('</svg>')
    svg_path.write_text("\n".join(parts), encoding="utf-8")

write_latency_qps_chart(latency_svg_path)
write_concurrency_qps_chart(throughput_svg_path)
PY
}

write_html_report() {
    python3 - "${RESULT_DIR}" "${REPORT_HTML}" "${STAMP}" <<'PY'
import csv
import html
import os
import sys
from pathlib import Path

result_dir = Path(sys.argv[1])
report_path = Path(sys.argv[2])
stamp = sys.argv[3]

def rel(path: Path) -> str:
    return html.escape(os.path.relpath(path, result_dir).replace(os.sep, "/"))

def read_csv_table(path: Path, max_rows: int = 24) -> str:
    if not path.exists():
        return "<p class='muted'>Missing.</p>"
    with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        return "<p class='muted'>Empty.</p>"
    head, body = rows[0], rows[1:max_rows + 1]
    more = max(0, len(rows) - 1 - len(body))
    parts = ["<div class='table-wrap'><table><thead><tr>"]
    parts.extend(f"<th>{html.escape(cell)}</th>" for cell in head)
    parts.append("</tr></thead><tbody>")
    for row in body:
        parts.append("<tr>")
        parts.extend(f"<td>{html.escape(cell)}</td>" for cell in row)
        parts.append("</tr>")
    parts.append("</tbody></table></div>")
    if more:
        parts.append(f"<p class='muted'>Preview shows {len(body)} rows; {more} more rows in <a href='{rel(path)}'>{html.escape(path.name)}</a>.</p>")
    return "".join(parts)

def svg_sort_key(path: Path) -> tuple[int, str]:
    order = {
        "concurrency-qps.svg": 0,
        "qps-latency.svg": 1,
        "likesprogram-resource.svg": 90,
        "likesprogram-resource-counts.svg": 91,
    }
    if path.name.startswith("fixed-qps-"):
        return (20, path.name)
    return (order.get(path.name, 50), path.name)

svgs = sorted(result_dir.glob("*.svg"), key=svg_sort_key)
csvs = sorted(result_dir.glob("*.csv"))
logs = sorted(list(result_dir.glob("*.log")) + list(result_dir.glob("*.wrk.log.warmup")))
metadata = result_dir / "run-metadata.txt"

cards = []
for svg in svgs:
    cards.append(
        f"<section class='chart'><h2>{html.escape(svg.stem)}</h2>"
        f"<a href='{rel(svg)}'><img src='{rel(svg)}' alt='{html.escape(svg.stem)}'></a></section>"
    )

links = []
for path in csvs + logs + ([metadata] if metadata.exists() else []):
    links.append(f"<li><a href='{rel(path)}'>{html.escape(path.name)}</a></li>")

html_text = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Net/Nginx benchmark {html.escape(stamp)}</title>
  <style>
    :root {{ color-scheme: light; font-family: Arial, sans-serif; }}
    body {{ margin: 0; background: #f8fafc; color: #111827; }}
    main {{ max-width: 1120px; margin: 0 auto; padding: 28px 20px 56px; }}
    header {{ margin-bottom: 24px; }}
    h1 {{ margin: 0 0 8px; font-size: 28px; line-height: 1.2; }}
    h2 {{ margin: 0 0 12px; font-size: 18px; }}
    p {{ line-height: 1.5; }}
    a {{ color: #005ea8; }}
    .muted {{ color: #4b5563; font-size: 13px; }}
    .grid {{ display: grid; gap: 18px; }}
    .chart, .data {{ background: #fff; border: 1px solid #dbe3ee; border-radius: 8px; padding: 16px; }}
    .chart img {{ display: block; width: 100%; height: auto; border: 1px solid #e5e7eb; border-radius: 4px; background: #fff; }}
    .table-wrap {{ overflow-x: auto; }}
    table {{ width: 100%; border-collapse: collapse; font-size: 13px; }}
    th, td {{ border-bottom: 1px solid #e5e7eb; padding: 7px 8px; text-align: right; white-space: nowrap; }}
    th:first-child, td:first-child {{ text-align: left; }}
    th {{ background: #f1f5f9; color: #111827; }}
    ul {{ columns: 2; padding-left: 20px; }}
    @media (max-width: 720px) {{ ul {{ columns: 1; }} main {{ padding: 20px 12px 40px; }} }}
  </style>
</head>
<body>
<main>
  <header>
    <h1>Net/Nginx benchmark {html.escape(stamp)}</h1>
    <p class="muted">All charts and raw artifacts for this benchmark round are preserved in this directory.</p>
  </header>
  <div class="grid">
    {''.join(cards) if cards else "<section class='chart'><h2>Charts</h2><p class='muted'>No SVG charts were generated.</p></section>"}
    <section class="data">
      <h2>QPS/latency data</h2>
      {read_csv_table(result_dir / "qps-latency.csv")}
    </section>
    <section class="data">
      <h2>Segmented QPS/latency data</h2>
      {read_csv_table(result_dir / "qps-latency-segments.csv")}
    </section>
    <section class="data">
      <h2>Fixed QPS summary</h2>
      {read_csv_table(result_dir / "fixed-qps-summary.csv")}
    </section>
    <section class="data">
      <h2>LikesProgramNet resource summary</h2>
      {read_csv_table(result_dir / "likesprogram-resource-summary.csv")}
    </section>
    <section class="data">
      <h2>Artifacts</h2>
      <ul>{''.join(links)}</ul>
    </section>
  </div>
</main>
</body>
</html>
"""
report_path.write_text(html_text, encoding="utf-8")
PY
}

write_run_metadata() {
    {
        echo "stamp=${STAMP}"
        echo "root_dir=${ROOT_DIR}"
        echo "build_dir=${BUILD_DIR}"
        echo "result_dir=${RESULT_DIR}"
        echo "bench_tool=${ACTUAL_TOOL}"
        echo "concurrency_points=${CONCURRENCY_POINTS}"
        echo "wrk_duration_seconds=${DURATION_SECONDS}"
        echo "wrk_segment_seconds=${SEGMENT_SECONDS}"
        echo "wrk_warmup_seconds=${WARMUP_SECONDS}"
        echo "wrk_threads=${WRK_THREADS}"
        echo "ab_requests=${REQUESTS}"
        echo "ab_warmup_requests=${WARMUP_REQUESTS}"
        echo "lp_port=${LP_PORT}"
        echo "nginx_port=${NGINX_PORT}"
        echo "lp_workers=${LP_WORKERS:-default}"
        echo "nginx_workers=${NGINX_WORKERS}"
        echo "lp_server_mode=${LP_SERVER_MODE}"
        echo "lp_bind_address=${LP_BIND_ADDRESS}"
        echo "fixed_qps_enable=${FIXED_QPS_ENABLE}"
        echo "fixed_qps_points=${FIXED_QPS_POINTS}"
        echo "fixed_qps_duration_seconds=${FIXED_QPS_DURATION_SECONDS}"
        echo "fixed_qps_workers=${FIXED_QPS_WORKERS}"
        echo "fixed_qps_engine=${FIXED_QPS_ENGINE}"
        echo "keep_result_runs=${KEEP_RESULT_RUNS}"
        echo "server_cpuset=${SERVER_CPUSET:-unbound}"
        echo "client_cpuset=${CLIENT_CPUSET:-unbound}"
        echo "resource_minimum_sample_percent=${RESOURCE_MINIMUM_SAMPLE_PERCENT}"
        echo
        echo "system:"
        uname -a || true
        echo "nproc=$(nproc 2>/dev/null || echo unknown)"
        grep -m1 'model name' /proc/cpuinfo 2>/dev/null || true
        grep -m1 MemTotal /proc/meminfo 2>/dev/null || true
        echo
        echo "tool versions:"
        nginx -v 2>&1 || true
        wrk --version 2>&1 || true
        hey -h 2>&1 | head -n 20 || true
        ab -V 2>&1 | head -n 1 || true
        python3 --version 2>&1 || true
    } > "${RESULT_DIR}/run-metadata.txt"
}

cleanup() {
    if [[ -n "${MONITOR_PID}" ]]; then
        touch "${RESOURCE_STOP}" >/dev/null 2>&1 || true
        wait "${MONITOR_PID}" >/dev/null 2>&1 || true
        MONITOR_PID=""
    fi
    if [[ -f "${RESOURCE_FILE}" && ! -f "${RESOURCE_SUMMARY}" ]]; then
        write_resource_summary >/dev/null 2>&1 || true
    fi
    if [[ -f "${RESOURCE_FILE}" && ! -f "${RESOURCE_SVG}" ]]; then
        write_resource_svg >/dev/null 2>&1 || true
    fi
    if [[ -f "${RESOURCE_FILE}" && ! -f "${RESOURCE_COUNTS_SVG}" ]]; then
        write_resource_counts_svg >/dev/null 2>&1 || true
    fi
    if [[ -f "${CSV_FILE}" && ( ! -f "${SVG_FILE}" || ! -f "${THROUGHPUT_SVG}" ) ]]; then
        write_svg >/dev/null 2>&1 || true
    fi
    if [[ ! -f "${REPORT_HTML}" ]]; then
        write_html_report >/dev/null 2>&1 || true
    fi
    if [[ -n "${LP_PID}" ]]; then
        kill "${LP_PID}" >/dev/null 2>&1 || true
        wait "${LP_PID}" >/dev/null 2>&1 || true
        LP_PID=""
    fi
    if [[ -f "${NGINX_PREFIX}/nginx.pid" ]]; then
        nginx -p "${NGINX_PREFIX}" -c "${NGINX_PREFIX}/nginx.conf" -s stop >/dev/null 2>&1 || true
    fi
}

# 无论成功、中断或工具失败都写诊断 manifest；完整失败也保留退出码并进入正式数据。
write_run_manifest() {
    local process_status="$1"
    local finished_at
    local finished_epoch
    local observed_duration
    local completed_sample_count=0
    local minimum_sample_count
    local completed="false"
    finished_at="$(date --iso-8601=seconds)"
    finished_epoch="$(date +%s)"
    observed_duration=$((finished_epoch - START_EPOCH))
    if [[ -f "${RESOURCE_FILE}" ]]; then
        completed_sample_count="$(awk 'END { print (NR > 0 ? NR - 1 : 0) }' "${RESOURCE_FILE}")"
    fi
    minimum_sample_count=$(((PLANNED_DURATION_SECONDS * RESOURCE_MINIMUM_SAMPLE_PERCENT + 99) / 100))
    if [[ "${RUN_FINISHED}" == "1" && "${COMPLETED_STAGE_COUNT}" == "${PLANNED_STAGE_COUNT}" ]]; then
        completed="true"
    fi

    python3 "${TEST_RECORD_TOOL}" \
        --write-manifest "${RESULT_DIR}/test-record.json" \
        --record-id "net-nginx-http-${VERSION}-${STAMP}" \
        --completed "${completed}" \
        --outcome "${TEST_OUTCOME}" \
        --version "${VERSION}" \
        --test-type "${TEST_TYPE}" \
        --platform "${TEST_PLATFORM}" \
        --backend "${TEST_BACKEND}" \
        --started-at "${STARTED_AT}" \
        --finished-at "${finished_at}" \
        --exit-code "${process_status}" \
        --planned-stage-count "${PLANNED_STAGE_COUNT}" \
        --completed-stage-count "${COMPLETED_STAGE_COUNT}" \
        --planned-duration-seconds "${PLANNED_DURATION_SECONDS}" \
        --observed-duration-seconds "${observed_duration}" \
        --planned-sample-count "${minimum_sample_count}" \
        --completed-sample-count "${completed_sample_count}" \
        --raw-glob "**/*.log*" \
        --raw-glob "*.csv" \
        --raw-glob "*.svg" \
        --raw-glob "run-metadata.txt" \
        --raw-glob "index.html" \
        --tabular-glob "*.csv" \
        --completion-evidence "stages=${COMPLETED_STAGE_COUNT}/${PLANNED_STAGE_COUNT}; process_exit=${process_status}; samples=${completed_sample_count}/${minimum_sample_count}; outcome=${TEST_OUTCOME}"
}

# 先收敛服务与资源采样，再生成单轮记录和全局看板。
on_exit() {
    local process_status="$?"
    local manifest_status
    local export_status
    trap - EXIT
    set +e
    cleanup
    write_run_manifest "${process_status}"
    manifest_status="$?"
    python3 "${TEST_RECORD_TOOL}" --records-root "${OUT_ROOT}" --output-dir "${OUT_ROOT}"
    export_status="$?"
    if [[ "${process_status}" == "0" && ( "${manifest_status}" != "0" || "${export_status}" != "0" ) ]]; then
        process_status=1
    fi
    exit "${process_status}"
}
trap on_exit EXIT

cat > "${NGINX_PREFIX}/nginx.conf" <<NGINX
worker_processes ${NGINX_WORKERS};
error_log ${NGINX_PREFIX}/logs/error.log warn;
pid ${NGINX_PREFIX}/nginx.pid;
events {
    worker_connections 8192;
    multi_accept on;
}
http {
    access_log off;
    sendfile on;
    tcp_nodelay on;
    keepalive_timeout 65;
    server_tokens off;
    server {
        listen 127.0.0.1:${NGINX_PORT};
        location / {
            default_type text/plain;
            return 200 "LikesProgramNet\n";
        }
    }
}
NGINX

if [[ ! -x "${LP_SERVER}" ]]; then
    echo "missing HttpBenchmarkServer: ${LP_SERVER}" >&2
    exit 1
fi
if ! command -v nginx >/dev/null 2>&1; then
    echo "missing nginx" >&2
    exit 1
fi
ACTUAL_TOOL="$(resolve_bench_tool)"
if [[ "${ACTUAL_TOOL}" != "ab" && "${ACTUAL_TOOL}" != "wrk" ]]; then
    echo "unsupported benchmark tool: ${ACTUAL_TOOL}" >&2
    exit 1
fi
if ! command -v "${ACTUAL_TOOL}" >/dev/null 2>&1; then
    echo "missing benchmark tool: ${ACTUAL_TOOL}" >&2
    exit 1
fi
if ! command -v ab >/dev/null 2>&1; then
    echo "missing ab" >&2
    exit 1
fi
if [[ ! -f "${WRK_RESULT_TOOL}" ]]; then
    echo "missing wrk result parser: ${WRK_RESULT_TOOL}" >&2
    exit 1
fi

# wrk 与固定速率工具有明确持续时间；ab 的请求数模式只依赖阶段计数。
if [[ "${ACTUAL_TOOL}" == "wrk" ]]; then
    PLANNED_DURATION_SECONDS=$((CONCURRENCY_STAGE_COUNT * 2 * DURATION_SECONDS))
fi
if [[ "${FIXED_QPS_ENABLE}" != "0" ]]; then
    PLANNED_DURATION_SECONDS=$((PLANNED_DURATION_SECONDS + FIXED_QPS_STAGE_COUNT * FIXED_QPS_DURATION_SECONDS))
fi
if [[ "${FIXED_QPS_ENABLE}" != "0" && ! -f "${FIXED_QPS_TOOL}" ]]; then
    echo "missing fixed QPS tool: ${FIXED_QPS_TOOL}" >&2
    exit 1
fi
if [[ "${FIXED_QPS_ENABLE}" != "0" && "${FIXED_QPS_ENGINE}" == "hey" ]] && ! command -v hey >/dev/null 2>&1; then
    echo "fixed QPS engine hey was requested but hey is not installed" >&2
    exit 1
fi

write_run_metadata

SERVER_LAUNCH=() # 服务端前缀只在显式设置 CPU 集时启用 taskset
if [[ -n "${SERVER_CPUSET}" ]]; then
    SERVER_LAUNCH=(taskset -c "${SERVER_CPUSET}")
fi

if [[ -n "${LP_WORKERS}" ]]; then
    "${SERVER_LAUNCH[@]}" env LD_LIBRARY_PATH="${LP_LIB_PATH}:${LD_LIBRARY_PATH:-}" "${LP_SERVER}" "${LP_PORT}" "${LP_WORKERS}" "${LP_SERVER_MODE}" "${LP_BIND_ADDRESS}" > "${RESULT_DIR}/likesprogram-server.log" 2>&1 &
else
    "${SERVER_LAUNCH[@]}" env LD_LIBRARY_PATH="${LP_LIB_PATH}:${LD_LIBRARY_PATH:-}" "${LP_SERVER}" "${LP_PORT}" "${LP_SERVER_MODE}" "${LP_BIND_ADDRESS}" > "${RESULT_DIR}/likesprogram-server.log" 2>&1 &
fi
LP_PID="$!"
"${SERVER_LAUNCH[@]}" nginx -p "${NGINX_PREFIX}" -c "${NGINX_PREFIX}/nginx.conf"

wait_http "http://127.0.0.1:${LP_PORT}/" "LikesProgramNet" "${LP_PID}"
wait_http "http://127.0.0.1:${NGINX_PORT}/" "Nginx"

echo "server,concurrency,qps,latency_ms,p50_ms,p90_ms,p99_ms,error_count,connect_error_count,read_error_count,write_error_count,timeout_error_count,non_success_error_count,tool,mode" > "${CSV_FILE}"
echo "server,concurrency,segment_index,segment_count,offset_seconds,duration_seconds,started_at,finished_at,qps,latency_ms,p50_ms,p90_ms,p99_ms,error_count,connect_error_count,read_error_count,write_error_count,timeout_error_count,non_success_error_count" > "${SEGMENT_CSV_FILE}"
rm -f "${RESOURCE_STOP}"
monitor_likesprogram_resource "${LP_PID}" "${RESOURCE_FILE}" "${RESOURCE_STOP}" &
MONITOR_PID="$!"

for concurrency in ${CONCURRENCY_POINTS}; do
    run_point "${ACTUAL_TOOL}" "LikesProgramNet" "http://127.0.0.1:${LP_PORT}/" "${concurrency}"
    ((COMPLETED_STAGE_COUNT += 1))
done

for concurrency in ${CONCURRENCY_POINTS}; do
    run_point "${ACTUAL_TOOL}" "Nginx" "http://127.0.0.1:${NGINX_PORT}/" "${concurrency}"
    ((COMPLETED_STAGE_COUNT += 1))
done

write_svg

if [[ "${FIXED_QPS_ENABLE}" != "0" ]]; then
    run_client python3 "${FIXED_QPS_TOOL}" \
        --out-dir "${RESULT_DIR}" \
        --duration "${FIXED_QPS_DURATION_SECONDS}" \
        --engine "${FIXED_QPS_ENGINE}" \
        --workers "${FIXED_QPS_WORKERS}" \
        --qps ${FIXED_QPS_POINTS} \
        --server "LikesProgramNet=http://127.0.0.1:${LP_PORT}/" \
        --server "Nginx=http://127.0.0.1:${NGINX_PORT}/"
    COMPLETED_STAGE_COUNT=$((COMPLETED_STAGE_COUNT + FIXED_QPS_STAGE_COUNT))
    if awk -F, 'NR > 1 && ($7 + 0) > 0 { found = 1 } END { exit(found ? 0 : 1) }' "${RESULT_DIR}/fixed-qps-summary.csv"; then
        TEST_OUTCOME="failed"
    fi
fi

touch "${RESOURCE_STOP}"
wait "${MONITOR_PID}" || true
MONITOR_PID=""
write_resource_summary
write_resource_svg
write_resource_counts_svg
write_html_report
RUN_FINISHED=1
echo "${RESULT_DIR}"
