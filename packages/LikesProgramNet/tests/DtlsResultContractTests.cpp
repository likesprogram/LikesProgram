#include <LikesProgram/Net/DtlsEngineFactory.hpp>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
    // 失败时立即终止，保持契约程序依赖最小。
    void Require(bool condition) {
        if (!condition) std::abort();
    }

    class ProbeDtlsEngine final : public LikesProgram::Net::DtlsEngine {
    public:
        // 生成固定首个 flight，并请求一次重传 timer。
        LikesProgram::Net::DtlsResult StartHandshake(
            LikesProgram::Net::DtlsDatagramBatch& ciphertextOutput) override {
            LikesProgram::Net::Buffer flight(0); // 测试握手密文数据报
            flight.Append("hello", 5);
            ciphertextOutput.Append(std::move(flight));
            m_state = LikesProgram::Net::DtlsState::Handshaking;
            return {
                LikesProgram::Net::DtlsAction::CiphertextReady
                    | LikesProgram::Net::DtlsAction::ArmRetransmitTimer,
                0,
                25
            };
        }

        // 消费完整密文数据报，测试不产生额外输出。
        LikesProgram::Net::DtlsResult ConsumeCiphertext(
            LikesProgram::Net::Buffer& ciphertextDatagram,
            LikesProgram::Net::DtlsDatagramBatch&,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            ciphertextDatagram.RetrieveAll();
            m_state = LikesProgram::Net::DtlsState::Active;
            return {};
        }

        // 消费完整业务数据报，测试不实现真实加密。
        LikesProgram::Net::DtlsResult ConsumePlaintext(
            LikesProgram::Net::Buffer& plaintextDatagram,
            LikesProgram::Net::DtlsDatagramBatch&) override {
            plaintextDatagram.RetrieveAll();
            return {};
        }

        // 重传 timer 到期后保持当前测试状态。
        LikesProgram::Net::DtlsResult HandleTimeout(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            return {};
        }

        // 关闭测试会话并请求移除当前 peer。
        LikesProgram::Net::DtlsResult Shutdown(
            LikesProgram::Net::DtlsDatagramBatch&) override {
            m_state = LikesProgram::Net::DtlsState::Closed;
            return { LikesProgram::Net::DtlsAction::CloseSession, 0, 0 };
        }

        // 返回当前测试 Engine 状态。
        LikesProgram::Net::DtlsState State() const noexcept override {
            return m_state;
        }

        // Active 后返回固定协商协议文本。
        const char* NegotiatedProtocol() const noexcept override {
            return m_state == LikesProgram::Net::DtlsState::Active ? "probe" : "";
        }

    private:
        LikesProgram::Net::DtlsState m_state = LikesProgram::Net::DtlsState::Handshaking; // 测试会话状态
    };
}

// 验证 DTLS action/result 与复制 Factory 的稳定公共契约。
int main() {
    const auto actions = LikesProgram::Net::DtlsAction::CiphertextReady
        | LikesProgram::Net::DtlsAction::ArmRetransmitTimer; // 可组合动作集合
    const LikesProgram::Net::DtlsResult result{ actions, 0, 25 }; // 成功且请求 timer 的结果
    Require(result.Succeeded());
    Require(result.HasAction(LikesProgram::Net::DtlsAction::CiphertextReady));
    Require(result.HasAction(LikesProgram::Net::DtlsAction::ArmRetransmitTimer));
    Require(!result.HasAction(LikesProgram::Net::DtlsAction::CloseSession));
    Require(!(LikesProgram::Net::DtlsResult{ {}, 5, 0 }).Succeeded());

    std::atomic<int> initialized{ 0 }; // 复制 Factory 共享的一次性初始化次数
    std::atomic<std::size_t> maximumSeen{ 0 }; // Create 回调接收的密文容量
    LikesProgram::Net::DtlsEngineFactory factory(
        LikesProgram::Net::DtlsRole::Server,
        [&maximumSeen](
            const LikesProgram::Net::Address&,
            const LikesProgram::Net::Address&,
            std::size_t maximumBytes) {
            maximumSeen.store(maximumBytes, std::memory_order_release);
            return std::make_unique<ProbeDtlsEngine>();
        },
        [&initialized]() {
            initialized.fetch_add(1, std::memory_order_relaxed);
            return true;
        });
    LikesProgram::Net::DtlsEngineFactory copied(factory); // 复制后共享初始化状态
    Require(factory.InitializeSharedResources());
    Require(copied.InitializeSharedResources());
    Require(initialized.load(std::memory_order_relaxed) == 1);
    Require(copied.Role() == LikesProgram::Net::DtlsRole::Server);

    LikesProgram::Net::Address peer; // 测试 peer 参数，平台无关契约无需建立系统地址
    LikesProgram::Net::Address local; // 测试 local 参数，平台无关契约无需建立系统地址
    auto engine = copied.Create(peer, local, 1200); // 每 peer 创建独立 Engine
    Require(static_cast<bool>(engine));
    Require(maximumSeen.load(std::memory_order_acquire) == 1200);

    LikesProgram::Net::DtlsDatagramBatch output; // 验证创建 Engine 的首个 flight
    const auto handshake = engine->StartHandshake(output);
    Require(handshake.HasAction(LikesProgram::Net::DtlsAction::CiphertextReady));
    Require(output.Count() == 1 && output.At(0).AsStringView() == "hello");

    LikesProgram::Net::DtlsEngineFactory throwingCreate(
        LikesProgram::Net::DtlsRole::Client,
        [](const LikesProgram::Net::Address&, const LikesProgram::Net::Address&, std::size_t)
            -> std::unique_ptr<LikesProgram::Net::DtlsEngine> {
            throw std::runtime_error("create failure");
        });
    Require(!throwingCreate.Create(peer, local, 1200));

    LikesProgram::Net::DtlsEngineFactory throwingInitializer(
        LikesProgram::Net::DtlsRole::Server,
        [](const LikesProgram::Net::Address&, const LikesProgram::Net::Address&, std::size_t) {
            return std::make_unique<ProbeDtlsEngine>();
        },
        []() -> bool {
            throw std::runtime_error("initialize failure");
        });
    Require(!throwingInitializer.InitializeSharedResources());

    LikesProgram::Net::DtlsEngineFactory empty; // 空 Factory 必须安全失败
    Require(!static_cast<bool>(empty));
    Require(empty.InitializeSharedResources());
    Require(!empty.Create(peer, local, 1200));
}
