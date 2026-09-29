#include <LikesProgram/Net/DtlsDatagramBatch.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
    // 失败时立即终止，保持契约程序依赖最小。
    void Require(bool condition) {
        if (!condition) std::abort();
    }
}

// 验证完整数据报边界、移动所有权和空数据报语义。
int main() {
    LikesProgram::Net::DtlsDatagramBatch batch; // 待验证的完整数据报队列
    Require(batch.Empty() && batch.Count() == 0 && batch.TotalBytes() == 0);

    LikesProgram::Net::Buffer first(0); // 首个非空数据报，验证移动后地址稳定
    first.Append("first", 5);
    LikesProgram::Net::Buffer empty(0); // 零长度数据报仍占一个边界
    LikesProgram::Net::Buffer third(0); // 尾部数据报验证 FIFO 顺序
    third.Append("third", 5);
    const auto* firstAddress = first.Peek(); // 移动前 payload 地址
    batch.Append(std::move(first));
    batch.Append(std::move(empty));
    batch.Append(std::move(third));

    Require(batch.Count() == 3 && batch.TotalBytes() == 10 && !batch.Empty());
    Require(batch.At(0).Peek() == firstAddress);
    Require(batch.At(1).ReadableBytes() == 0);
    Require(batch.At(2).AsStringView() == std::string_view("third"));

    LikesProgram::Net::Buffer output(0); // 接收队首所有权的复用 Buffer
    Require(batch.TakeFront(output));
    Require(output.AsStringView() == std::string_view("first"));
    Require(batch.Count() == 2 && batch.TotalBytes() == 5);
    batch.Clear();
    Require(batch.Empty() && !batch.TakeFront(output));

    bool threw = false; // 越界访问必须具有稳定异常边界
    try {
        (void)batch.At(0);
    }
    catch (const std::out_of_range&) {
        threw = true;
    }
    Require(threw);
}
