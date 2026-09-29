#pragma once

namespace LikesProgram {
    namespace Net {
        namespace Internal {
            // 判断已提交 operation 是否需要继续尝试生成 cancel SQE。
            inline bool NeedsOperationCancellation(
                bool outstanding,
                bool cancelRequested,
                bool operationWanted) noexcept {
                return outstanding && !cancelRequested && !operationWanted;
            }

            // multishot 仍存活时保留已提交 cancel，terminal CQE 才允许解除 latch。
            inline bool RetainCancellationRequest(
                bool cancelRequested,
                bool operationHasMore) noexcept {
                return cancelRequested && operationHasMore;
            }
        }
    }
}
