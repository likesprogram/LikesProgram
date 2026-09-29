#include <LikesProgram/Quic/QuicEngine.hpp>
#include <LikesProgram/Quic/QuicEngineFactory.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace LikesProgram::Quic;

    class FactoryProbeEngine final : public QuicEngine {
    public:
        explicit FactoryProbeEngine(QuicEngineOptions options)
            : m_role(options.role), m_applicationProtocol(options.applicationProtocol) {
        }

        void SetActionSink(QuicActionSink*) noexcept override {
        }

        void SetEventObserver(QuicEventObserver*) noexcept override {
        }

        void SetPacketProtectionProvider(QuicPacketProtectionProvider*) noexcept override {
        }

        QuicPacketProtectionResult ProtectPacket(
            const QuicPacketProtectionRequest&, std::span<std::uint8_t>) noexcept override {
            return { QuicPacketProtectionError::NotReady, 0 };
        }

        QuicPacketProtectionResult UnprotectPacket(
            const QuicPacketProtectionRequest&, std::span<std::uint8_t>) noexcept override {
            return { QuicPacketProtectionError::NotReady, 0 };
        }

        QuicResult StartHandshake() override {
            return {};
        }

        QuicResult ProvideTlsHandshakeResult(const QuicTlsHandshakeResult&) override {
            return {};
        }

        QuicResult ConsumeDatagram(const Address&, Buffer&& datagram) override {
            datagram.RetrieveAll();
            return {};
        }

        QuicResult HandleTimeout() override {
            return {};
        }

        QuicResult SendStreamData(std::uint64_t, Buffer&& plaintext) override {
            plaintext.RetrieveAll();
            return {};
        }

        QuicResult SendStreamFin(std::uint64_t) override {
            return {};
        }

        QuicResult ResetStream(std::uint64_t, std::uint64_t) override {
            return {};
        }

        QuicResult StopSending(std::uint64_t, std::uint64_t) override {
            return {};
        }

        QuicResult Close(std::uint64_t) override {
            return {};
        }

        QuicState State() const noexcept override {
            return QuicState::Handshaking;
        }

        QuicEngineSnapshot Snapshot() const noexcept override {
            return {};
        }

        const char* NegotiatedProtocol() const noexcept override {
            return m_applicationProtocol.c_str();
        }

        QuicTlsVersion NegotiatedTlsVersion() const noexcept override {
            return QuicTlsVersion::Unknown;
        }

        QuicRole Role() const noexcept {
            return m_role;
        }

    private:
        QuicRole m_role = QuicRole::Client;
        std::string m_applicationProtocol;
    };

    void Require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    QuicEngineOptions ValidOptions() {
        QuicEngineOptions options;
        options.role = QuicRole::Server;
        options.applicationProtocol = "h3";
        options.maximumDatagramBytes = 1450;
        options.idleTimeout = std::chrono::milliseconds(2500);
        return options;
    }
}

int main() {
    try {
        const auto validOptions = ValidOptions();
        QuicEngineFactory empty;
        Require(!empty, "empty factory reports callable");
        Require(empty.InitializeSharedResources(), "empty initializer should succeed");
        Require(!empty.Create(validOptions), "empty factory created an engine");

        std::atomic<int> createCalls{ 0 };
        QuicEngineOptions receivedOptions;
        QuicEngineFactory factory(
            [&](const QuicEngineOptions& options) {
                ++createCalls;
                receivedOptions = options;
                return std::make_unique<FactoryProbeEngine>(options);
            });
        Require(static_cast<bool>(factory), "callback factory reports empty");

        auto invalidRole = validOptions;
        invalidRole.role = static_cast<QuicRole>(255);
        Require(!factory.Create(invalidRole), "invalid role reached callback");
        auto invalidTls = validOptions;
        invalidTls.tlsVersion = QuicTlsVersion::Unknown;
        Require(!factory.Create(invalidTls), "invalid TLS reached callback");
        auto invalidAlpn = validOptions;
        invalidAlpn.applicationProtocol = nullptr;
        Require(!factory.Create(invalidAlpn), "null ALPN reached callback");
        auto invalidDatagram = validOptions;
        invalidDatagram.maximumDatagramBytes = 1199;
        Require(!factory.Create(invalidDatagram), "small datagram reached callback");
        auto invalidTimeout = validOptions;
        invalidTimeout.idleTimeout = std::chrono::milliseconds(-1);
        Require(!factory.Create(invalidTimeout), "negative timeout reached callback");
        Require(createCalls.load() == 0, "invalid options invoked callback");

        auto first = factory.Create(validOptions);
        auto second = factory.Create(validOptions);
        Require(first && second && first.get() != second.get(), "engines are not independent");
        auto* firstProbe = dynamic_cast<FactoryProbeEngine*>(first.get());
        Require(firstProbe && firstProbe->Role() == QuicRole::Server,
            "callback did not receive options");
        Require(std::string_view(firstProbe->NegotiatedProtocol()) == "h3",
            "callback option protocol was not retained");
        Require(receivedOptions.maximumDatagramBytes == 1450,
            "callback option datagram size was not observed");
        Require(createCalls.load() == 2, "valid callback count mismatch");

        QuicEngineFactory copied(factory);
        QuicEngineFactory moved(std::move(copied));
        Require(!copied && moved, "copy/move factory state mismatch");
        Require(moved.Create(validOptions) != nullptr, "moved factory cannot create");
        QuicEngineFactory assigned;
        assigned = moved;
        Require(assigned && assigned.Create(validOptions) != nullptr,
            "copy assignment lost callback");
        QuicEngineFactory moveAssigned;
        moveAssigned = std::move(assigned);
        Require(!assigned && moveAssigned && moveAssigned.Create(validOptions) != nullptr,
            "move assignment lost callback");
        Require(createCalls.load() == 5, "copy/move callback sharing mismatch");

        std::atomic<int> initializerCalls{ 0 };
        QuicEngineFactory initialized(
            [](const QuicEngineOptions&) {
                return std::make_unique<FactoryProbeEngine>(ValidOptions());
            },
            [&]() {
                ++initializerCalls;
                return true;
            });
        QuicEngineFactory initializedCopy = initialized;
        QuicEngineFactory initializedMove = std::move(initializedCopy);
        std::atomic<int> successfulInitializers{ 0 };
        std::thread workers[8];
        for (auto& worker : workers) {
            worker = std::thread([&]() {
                if (initializedMove.InitializeSharedResources()) ++successfulInitializers;
            });
        }
        for (auto& worker : workers) worker.join();
        Require(successfulInitializers.load() == 8, "shared initializer did not succeed");
        Require(initializerCalls.load() == 1, "shared initializer ran more than once");
        Require(initializedMove.InitializeSharedResources(), "cached initializer result changed");
        Require(initializerCalls.load() == 1, "cached initializer ran again");

        std::atomic<int> rejectedInitializerCalls{ 0 };
        QuicEngineFactory rejectedInitializer(
            [](const QuicEngineOptions&) {
                return std::make_unique<FactoryProbeEngine>(ValidOptions());
            },
            [&]() {
                ++rejectedInitializerCalls;
                return false;
            });
        QuicEngineFactory rejectedCopy = rejectedInitializer;
        Require(!rejectedCopy.InitializeSharedResources(), "false initializer succeeded");
        Require(!rejectedInitializer.InitializeSharedResources(), "false result was not shared");
        Require(rejectedInitializerCalls.load() == 1, "false initializer ran more than once");

        std::atomic<int> throwingInitializerCalls{ 0 };
        QuicEngineFactory throwingInitializer(
            [](const QuicEngineOptions&) {
                return std::make_unique<FactoryProbeEngine>(ValidOptions());
            },
            [&]() -> bool {
                ++throwingInitializerCalls;
                throw std::runtime_error("initializer failure");
            });
        Require(!throwingInitializer.InitializeSharedResources(), "throwing initializer escaped");
        Require(!throwingInitializer.InitializeSharedResources(), "throwing result was not cached");
        Require(throwingInitializerCalls.load() == 1, "throwing initializer ran more than once");

        QuicEngineFactory throwingCreate(
            [](const QuicEngineOptions&) -> std::unique_ptr<QuicEngine> {
                throw std::runtime_error("create failure");
            });
        Require(throwingCreate && !throwingCreate.Create(validOptions),
            "throwing create callback escaped or returned an engine");

        std::cout << "passed=true empty=true invalid_rejected=true callback_exception=true"
                  << " independent_engines=true copy_move_shared=true"
                  << " initializer_once=true initializer_concurrent=true"
                  << " initializer_false_cached=true initializer_exception_cached=true"
                  << " options_forwarded=true\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "passed=false error=" << error.what() << '\n';
        return 1;
    }
}
