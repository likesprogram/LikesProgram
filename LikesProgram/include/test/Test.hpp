#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include "../LikesProgram/time/Timer.hpp"
#include <windows.h>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

using Microsoft::WRL::ComPtr;

#ifdef _WIN32
#include <psapi.h>
size_t GetRssKB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / 1024;
    }
    return 0;
}
#else
#include <fstream>
#include <unistd.h>
size_t GetRssKB() {
    std::ifstream f("/proc/self/statm");
    long size, resident;
    if (!(f >> size >> resident)) return 0;
    long page = sysconf(_SC_PAGESIZE) / 1024;
    return resident * page;
}
#endif

class Test {
public:
    explicit Test(const std::wstring& name, uint64_t maxPoints = 10000)
        : testName(name), maxPoints(maxPoints), running(true) {
    }

    ~Test() { Shutdown(); }

    void showWindow(uint64_t* testIndex, const double fps = 30) {
        currentFps.store(-2, std::memory_order_relaxed);
        lastUpdate = std::chrono::system_clock::now();

        timeBeginPeriod(1);
        windowThread = std::thread([this, testIndex, fps]() {
            const wchar_t CLASS_NAME[] = L"TestWindowClass";

            WNDCLASS wc{};
            wc.lpfnWndProc = WindowProcStatic;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = CLASS_NAME;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClass(&wc);

            hwnd = CreateWindowExW(
                0, CLASS_NAME, testName.c_str(),
                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                CW_USEDEFAULT, CW_USEDEFAULT, 1600, 900,
                nullptr, nullptr, GetModuleHandleW(nullptr), this
            );

            InitD2D();

            MSG msg{};
            int64_t lastIndex = -1;
            int64_t lastRSS = 0;

            LikesProgram::Time::Timer timer;
            const double frameDurationNs = 1e9 / fps;
            int64_t errorNs = 2'000'0;
            while (running) {
                timer.Start();
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                if (*testIndex != lastIndex) {
                    lastIndex = *testIndex;
                    int64_t newRSS = GetRssKB();
                    int64_t delta = (lastRSS > 0) ? newRSS - lastRSS : 0;
                    lastRSS = newRSS;

                    rssHistory.push_back(newRSS);
                    deltaHistory.push_back(delta);

                    if (!rssHistory.empty()) {
                        size_t sumRss = 0;
                        for (auto v : rssHistory) sumRss += v;
                        avgRssHistory.push_back(sumRss / rssHistory.size());
                    }

                    if (!deltaHistory.empty()) {
                        int64_t sumDelta = 0;
                        for (auto d : deltaHistory) sumDelta += d;
                        avgDeltaHistory.push_back(sumDelta / deltaHistory.size());
                    }

                    if (rssHistory.size() > maxPoints) rssHistory.erase(rssHistory.begin());
                    if (deltaHistory.size() > maxPoints) deltaHistory.erase(deltaHistory.begin());
                    if (avgRssHistory.size() > maxPoints) avgRssHistory.erase(avgRssHistory.begin());
                    if (avgDeltaHistory.size() > maxPoints) avgDeltaHistory.erase(avgDeltaHistory.begin());

                    // 最后更新时间
                    lastUpdate = std::chrono::system_clock::now();
                }

                // 绘制
                DrawD2D(*testIndex);

                static int64_t frameCount = 0;

                // 计算 FPS
                static LikesProgram::Time::Timer frameTimer(true);
                auto frameElapsed = frameTimer.Stop();
                frameTimer.Reset();
                frameTimer.Start();

                // 控制 FPS
                auto elapsed = timer.Stop(); // 纳秒耗时
                int64_t elapsedNs = elapsed.count();

                // 每秒更新 3 次帧率
                if (++frameCount % (int)(fps/ 3) == 0) {
                    double prev = currentFps.load(std::memory_order_relaxed);
                    if (prev <= 0) currentFps.store(1e9 / (double)frameElapsed.count(), std::memory_order_relaxed);
                    if (frameElapsed.count() > 0 && prev > -1) {
                        double next = Math::EMA(prev, 1e9 / (double)frameElapsed.count(), 0.9);
                        while (!currentFps.compare_exchange_weak(prev, next, std::memory_order_relaxed)) {
                            next = Math::EMA(prev, 1e9 / (double)frameElapsed.count(), 0.9);
                        }
                    }
                }

                if (elapsedNs < frameDurationNs) {
                    auto remain = frameDurationNs - elapsedNs;
                    if (remain > errorNs) std::this_thread::sleep_for(std::chrono::nanoseconds((int64_t)remain - errorNs));
                    else {
                        timer.Reset();
                        timer.Start();
                        while (timer.Stop().count() < frameDurationNs) {
                            std::this_thread::yield();
                            timer.Reset();
                            timer.Start();
                        }
                    }
                }
            }
        });
    }

    void Shutdown() {
        running = false;
        timeEndPeriod(1);
        if (windowThread.joinable()) windowThread.join();
    }

private:
    std::wstring testName;
    std::atomic<bool> running;
    std::thread windowThread;
    HWND hwnd = nullptr;

    // D2D
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<ID2D1HwndRenderTarget> renderTarget;
    ComPtr<ID2D1SolidColorBrush> brushRss, brushRssAvg, brushDelta, brushDeltaAvg, brushAxis, brushGrid, brushZero;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> textFormat;
    ComPtr<ID2D1BitmapRenderTarget> offscreenRss;
    ComPtr<ID2D1BitmapRenderTarget> offscreenDelta;
    std::atomic<double> currentFps{ - 1.0 };
    uint64_t maxPoints = 0;
    LikesProgram::Time::TimePoint lastUpdate;
    std::vector<size_t> rssHistory;
    std::vector<int64_t> deltaHistory;
    std::vector<size_t> avgRssHistory;
    std::vector<int64_t> avgDeltaHistory;

    void InitD2D() {
        D2D1_FACTORY_OPTIONS options{};
        options.debugLevel = D2D1_DEBUG_LEVEL_NONE; // 可改 D2D1_DEBUG_LEVEL_INFORMATION 调试

        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),
            &options,
            reinterpret_cast<void**>(d2dFactory.GetAddressOf())
        );

        if (FAILED(hr)) {
            MessageBox(nullptr, L"Failed to create Direct2D factory.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        // DirectWrite 工厂
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())
        );
        if (FAILED(hr)) {
            MessageBox(nullptr, L"Failed to create DirectWrite factory.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        // 显式硬件渲染属性
        D2D1_RENDER_TARGET_PROPERTIES props =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_HARDWARE,  // 强制硬件
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,     // 常用格式
                    D2D1_ALPHA_MODE_IGNORE          // 忽略透明
                ),
                96.0f, 96.0f,                       // DPI
                D2D1_RENDER_TARGET_USAGE_NONE,
                D2D1_FEATURE_LEVEL_DEFAULT
            );

        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(hwnd, size);
        hwndProps.presentOptions = D2D1_PRESENT_OPTIONS_IMMEDIATELY; // 禁用 VSync

        hr = d2dFactory->CreateHwndRenderTarget(
            props,
            hwndProps,
            &renderTarget
        );

        if (FAILED(hr)) {
            MessageBox(nullptr, L"Failed to create HwndRenderTarget.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        // 画刷
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Blue), &brushRss);
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green), &brushRssAvg);
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &brushDelta);
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DarkOrange), &brushDeltaAvg);
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brushAxis);
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &brushGrid);
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Purple), &brushZero);

        // 文本格式
        writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f,
            L"en-us",
            &textFormat
        );

        // 离屏缓存
        renderTarget->CreateCompatibleRenderTarget(D2D1::SizeF((float)(rc.right - rc.left), (float)(rc.bottom - rc.top)), &offscreenRss);
        renderTarget->CreateCompatibleRenderTarget(D2D1::SizeF((float)(rc.right - rc.left), (float)(rc.bottom - rc.top)), &offscreenDelta);
    }

    void DrawLegend(ID2D1RenderTarget* target) {
        if (!target) return;
        float startX = 60, startY = 40, spacing = 150;

        D2D1_RECT_F layoutRect = D2D1::RectF(startX + 25, startY - 10, startX + 120, startY + 10);
        target->DrawLine(D2D1::Point2F(startX, startY), D2D1::Point2F(startX + 20, startY), brushRss.Get(), 1.0f);
        target->DrawText(L"Current RSS", 11, textFormat.Get(), &layoutRect, brushAxis.Get());

        layoutRect = D2D1::RectF(startX + spacing + 25, startY - 10, startX + spacing + 100, startY + 10);
        target->DrawLine(D2D1::Point2F(startX + spacing, startY), D2D1::Point2F(startX + spacing + 20, startY), brushRssAvg.Get(), 1.0f);
        target->DrawText(L"Avg RSS", 7, textFormat.Get(), &layoutRect, brushAxis.Get());

        layoutRect = D2D1::RectF(startX + 2 * spacing + 25, startY - 10, startX + 2 * spacing + 100, startY + 10);
        target->DrawLine(D2D1::Point2F(startX + 2 * spacing, startY), D2D1::Point2F(startX + 2 * spacing + 20, startY), brushDelta.Get(), 1.0f);
        target->DrawText(L"Delta RSS", 9, textFormat.Get(), &layoutRect, brushAxis.Get());

        layoutRect = D2D1::RectF(startX + 3 * spacing + 25, startY - 10, startX + 3 * spacing + 100, startY + 10);
        target->DrawLine(D2D1::Point2F(startX + 3 * spacing, startY), D2D1::Point2F(startX + 3 * spacing + 20, startY), brushDeltaAvg.Get(), 1.0f);
        target->DrawText(L"Avg Delta", 9, textFormat.Get(), &layoutRect, brushAxis.Get());
    }

    void DrawFPS(ID2D1RenderTarget* target, int width) {
        if (!target) return;
        // 绘制 FPS (右上角，绿色)
        std::wstringstream fpsText;
        fpsText.precision(2);
        fpsText << std::fixed << L"FPS: " << currentFps.load(std::memory_order_relaxed);

        D2D1_RECT_F fpsRect = D2D1::RectF((float)(width - 120), 10.0f, (float)(width - 10), 40.0f);

        ComPtr<ID2D1SolidColorBrush> brushFps;
        target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green), &brushFps);

        target->DrawText(
            fpsText.str().c_str(),
            (UINT32)fpsText.str().length(),
            textFormat.Get(),
            &fpsRect,
            brushFps.Get()
        );
    }

    void DrawStatsText(ID2D1RenderTarget* target, int width, uint64_t testIndex) {
        if (!target) return;
        // 静态起始时间，仅第一次调用时初始化
        static auto startTime = std::chrono::steady_clock::now();

        // 计算运行时间
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
        int hours = (int)(elapsed / 3600);
        int minutes = (int)((elapsed % 3600) / 60);
        int seconds = (int)(elapsed % 60);

        std::wstringstream timeStream;
        timeStream << std::setw(2) << std::setfill(L'0') << hours << L":"
            << std::setw(2) << std::setfill(L'0') << minutes << L":"
            << std::setw(2) << std::setfill(L'0') << seconds;

        std::wstringstream ss;
        ss << L"[Uptime " << timeStream.str() << L"]  ";
        ss << testName << L" Test 【" << testIndex
            << L" | Updated: " << LikesProgram::Time::FormatTime(lastUpdate).ToWString() << L"】 ： ";
        if (!rssHistory.empty()) ss << L"Current RSS: " << rssHistory.back();
        if (!deltaHistory.empty()) ss << L" KB； Delta: " << deltaHistory.back();
        if (!avgRssHistory.empty()) ss << L" KB； Avg RSS: " << avgRssHistory.back();
        if (!avgDeltaHistory.empty()) ss << L" KB； Avg Delta: " << avgDeltaHistory.back() << L" KB";

        D2D1_RECT_F layoutRect = D2D1::RectF(50, 10, (FLOAT)width - 50, 50);
        target->DrawText(ss.str().c_str(), (UINT32)ss.str().length(), textFormat.Get(), &layoutRect, brushAxis.Get());
    }

    void DrawMemoryPlot(ID2D1RenderTarget* target, int width, int height, int marginLeft, int marginRight, int marginTop, int marginBottom, int axisMargin, int plotWidth, int plotHeight) {
        if (!target) return;

        int n = (int)rssHistory.size();
        if (n == 0) return;

        size_t maxRss = *std::max_element(rssHistory.begin(), rssHistory.end());
        size_t minRss = 0;

        int64_t maxDelta = 0, minDelta = 0;
        if (!deltaHistory.empty()) {
            maxDelta = *std::max_element(deltaHistory.begin(), deltaHistory.end());
            minDelta = *std::min_element(deltaHistory.begin(), deltaHistory.end());
        }
        if (maxDelta == minDelta) { maxDelta += 1; minDelta -= 1; }

        // 背景网格
        for (int i = 0; i <= 5; ++i) {
            float y = marginTop + i * plotHeight / 5.0f;
            target->DrawLine(
                D2D1::Point2F((float)marginLeft + axisMargin, y),
                D2D1::Point2F((float)marginLeft + axisMargin + plotWidth, y),
                brushGrid.Get(), 1.0f
            );
        }

        // 总量曲线
        for (int i = 1; i < n; ++i) {
            float x1 = marginLeft + axisMargin + (i - 1) * plotWidth / (float)n;
            float y1 = marginTop + (float)(plotHeight * (maxRss - rssHistory[i - 1]) / (double)(maxRss - minRss));
            float x2 = marginLeft + axisMargin + i * plotWidth / (float)n;
            float y2 = marginTop + (float)(plotHeight * (maxRss - rssHistory[i]) / (double)(maxRss - minRss));
            target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brushRss.Get(), 1.0f);
        }

        // 平均总量曲线
        for (int i = 1; i < avgRssHistory.size(); ++i) {
            float x1 = marginLeft + axisMargin + (i - 1) * plotWidth / (float)avgRssHistory.size();
            float y1 = marginTop + (float)(plotHeight * (maxRss - avgRssHistory[i - 1]) / (double)(maxRss - minRss));
            float x2 = marginLeft + axisMargin + i * plotWidth / (float)avgRssHistory.size();
            float y2 = marginTop + (float)(plotHeight * (maxRss - avgRssHistory[i]) / (double)(maxRss - minRss));
            target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brushRssAvg.Get(), 1.0f);
        }

        // 增量曲线
        for (int i = 1; i < deltaHistory.size(); ++i) {
            float x1 = marginLeft + axisMargin + (i - 1) * plotWidth / (float)deltaHistory.size();
            float y1 = marginTop + (float)(plotHeight * (maxDelta - deltaHistory[i - 1]) / (double)(maxDelta - minDelta));
            float x2 = marginLeft + axisMargin + i * plotWidth / (float)deltaHistory.size();
            float y2 = marginTop + (float)(plotHeight * (maxDelta - deltaHistory[i]) / (double)(maxDelta - minDelta));
            target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brushDelta.Get(), 1.0f);
        }

        // 平均增量曲线
        for (int i = 1; i < avgDeltaHistory.size(); ++i) {
            float x1 = marginLeft + axisMargin + (i - 1) * plotWidth / (float)avgDeltaHistory.size();
            float y1 = marginTop + (float)(plotHeight * (maxDelta - avgDeltaHistory[i - 1]) / (double)(maxDelta - minDelta));
            float x2 = marginLeft + axisMargin + i * plotWidth / (float)avgDeltaHistory.size();
            float y2 = marginTop + (float)(plotHeight * (maxDelta - avgDeltaHistory[i]) / (double)(maxDelta - minDelta));
            target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brushDeltaAvg.Get(), 1.0f);
        }

        // 左侧纵轴标签 (Total RSS)
        for (int i = 0; i <= 5; ++i) {
            size_t val = maxRss - maxRss * i / 5;
            D2D1_RECT_F axisRect = D2D1::RectF((FLOAT)axisMargin, marginTop + i * plotHeight / 5.0f - 7.0f, 50.0f, marginTop + i * plotHeight / 5.0f + 7.0f);
            std::wstring strVal = std::to_wstring(val);
            target->DrawText(strVal.c_str(), (UINT32)strVal.length(), textFormat.Get(), &axisRect, brushAxis.Get());
        }

        // 右侧纵轴标签 (Delta RSS)
        for (int i = 0; i <= 5; ++i) {
            int64_t val = maxDelta - (maxDelta - minDelta) * i / 5;
            D2D1_RECT_F axisRect = D2D1::RectF((float)(width - marginRight + axisMargin), marginTop + i * plotHeight / 5.0f - 7.0f,
                (float)width, marginTop + i * plotHeight / 5.0f + 7.0f);
            std::wstring strVal = std::to_wstring(val);
            target->DrawText(strVal.c_str(), (UINT32)strVal.length(), textFormat.Get(), &axisRect, brushAxis.Get());
        }

        // 增量零线
        if (minDelta < 0 && maxDelta > 0) {
            float y0 = marginTop + (float)((maxDelta / (double)(maxDelta - minDelta)) * plotHeight);
            target->DrawLine(D2D1::Point2F((float)marginLeft + axisMargin, y0),
                D2D1::Point2F((float)marginLeft + axisMargin + plotWidth, y0),
                brushZero.Get(), 1.0f);
            D2D1_RECT_F zeroRect = D2D1::RectF((float)(width - marginRight + axisMargin), y0 - 7.0f, (float)width, y0 + 7.0f);
            target->DrawText(L"0", 1, textFormat.Get(), &zeroRect, brushZero.Get());
        }
    }

    void DrawD2D(uint64_t testIndex) {
        if (!renderTarget) return;
        renderTarget->BeginDraw();
        renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::White));

        RECT rc;
        GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        // 边距和绘图尺寸
        int marginLeft = 50, marginRight = 50, marginTop = 70, marginBottom = 30, axisMargin = 10;
        int plotWidth = width - marginLeft - marginRight - axisMargin;
        int plotHeight = height - marginTop - marginBottom;

        offscreenRss->BeginDraw();
        offscreenRss->Clear(D2D1::ColorF(D2D1::ColorF::White));

        // 绘制 内存曲线
        DrawMemoryPlot(offscreenRss.Get(), width, height, marginLeft, marginRight, marginTop, marginBottom, axisMargin, plotWidth, plotHeight);
        DrawLegend(offscreenRss.Get());      // 绘制图例
        DrawStatsText(offscreenRss.Get(), width, testIndex); // 绘制统计文字
        DrawFPS(offscreenRss.Get(), width);  // 绘制 FPS
        HRESULT hr = offscreenRss->EndDraw();
        if (FAILED(hr)) { }

        // 把 offscreenRss 绘制到窗口
        ComPtr<ID2D1Bitmap> bitmap;
        offscreenRss->GetBitmap(&bitmap);
        renderTarget->DrawBitmap(bitmap.Get(),
            nullptr, // 默认绘制整个 bitmap
            1.0f,    // 透明度
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        renderTarget->EndDraw();
    }

    static LRESULT CALLBACK WindowProcStatic(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_NCCREATE) {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        }
        Test* self = reinterpret_cast<Test*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (self) return self->WindowProc(hwnd, uMsg, wParam, lParam);
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    LRESULT WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        switch (uMsg) {
        case WM_SIZE:
            if (renderTarget) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
                renderTarget->Resize(size);

                // 重新创建离屏缓冲区
                offscreenRss.Reset();
                offscreenDelta.Reset();
                renderTarget->CreateCompatibleRenderTarget(
                    D2D1::SizeF((float)size.width, (float)size.height),
                    &offscreenRss
                );
                renderTarget->CreateCompatibleRenderTarget(
                    D2D1::SizeF((float)size.width, (float)size.height),
                    &offscreenDelta
                );
            }
            return 0;
        case WM_DESTROY:
            running = false;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
};
