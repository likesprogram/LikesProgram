#include <LikesProgram/Http/Http3.hpp>
#include <LikesProgram/Http/Http3QuicAdapter.hpp>
#include <LikesProgram/Quic/QuicStreamControlFrame.hpp>
#include <LikesProgram/Quic/QuicStreamFrame.hpp>
#include <LikesProgram/Quic/QuicVarInt.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace {
    using LikesProgram::Http::Http3QuicAction;
    using LikesProgram::Http::Http3QuicActionKind;
    using LikesProgram::Http::Http3QuicAdapter;
    using LikesProgram::Http::Http3QuicActionSink;
    using LikesProgram::Quic::QuicResetStreamFrame;
    using LikesProgram::Quic::QuicStreamFrame;
    using LikesProgram::Quic::QuicStreamFrameBuildOptions;

    constexpr std::uint64_t kMaximumQuicVarInt =
        LikesProgram::Quic::kQuicVarIntMaximum;

    struct WireRecord {
        std::uint64_t actionId = 0;
        std::vector<std::uint8_t> bytes;
    };

    class ActionRecorder final : public Http3QuicActionSink {
    public:
        void Submit(const Http3QuicAction& action) noexcept override {
            try {
                actions.push_back(action);
            }
            catch (...) {
                failed = true;
            }
        }

        std::vector<Http3QuicAction> actions;
        bool failed = false;
    };

    class CallerWireCoordinateMapper {
    public:
        LikesProgram::Result<std::vector<std::uint8_t>> MapStreamAction(
            const Http3QuicAction& action,
            std::uint64_t offset,
            bool fin) {
            if (action.actionId == 0 || action.streamId > kMaximumQuicVarInt
                || ((action.kind == Http3QuicActionKind::StreamData)
                    ? action.payload.size() > kMaximumQuicVarInt
                    : action.kind != Http3QuicActionKind::StreamFin
                        || !action.payload.empty())) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/3 DATA coordinate action is invalid");
            }
            if (action.kind == Http3QuicActionKind::StreamFin && !fin) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/3 FIN coordinate must set STREAM FIN");
            }

            const auto found = m_streams.find(action.streamId);
            StreamState state = found == m_streams.end()
                ? StreamState{}
                : found->second;
            if (state.finalKnown || offset != state.nextOffset
                || offset > kMaximumQuicVarInt
                || action.payload.size() > kMaximumQuicVarInt - offset
                || (fin && offset + action.payload.size() > kMaximumQuicVarInt)) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/3 DATA coordinate is non-monotonic or overflows");
            }

            const QuicStreamFrameBuildOptions options{
                action.streamId, offset, true, true, fin,
                { action.payload.data(), action.payload.size() } };
            const auto encoded = LikesProgram::Quic::BuildQuicStreamFrame(options);
            if (!encoded.IsOk()) return encoded.GetStatus();
            const auto parsed = LikesProgram::Quic::ParseQuicStreamFrame(
                encoded.Value().data(), encoded.Value().size());
            if (!parsed.IsOk() || parsed.Value().streamId != action.streamId
                || parsed.Value().offset != offset
                || parsed.Value().fin != fin
                || parsed.Value().data.size() != action.payload.size()) {
                return LikesProgram::Status::Internal(
                    u"QUIC DATA coordinate round trip changed the action");
            }

            state.nextOffset = offset + action.payload.size();
            if (fin) {
                state.finalKnown = true;
                state.finalSize = state.nextOffset;
            }
            m_records.push_back({ action.actionId, encoded.Value() });
            m_streams.insert_or_assign(action.streamId, state);
            return encoded.Value();
        }

        LikesProgram::Result<std::vector<std::uint8_t>> MapResetAction(
            const Http3QuicAction& action,
            std::uint64_t finalSize) {
            if (action.actionId == 0
                || action.kind != Http3QuicActionKind::ResetStream
                || action.streamId > kMaximumQuicVarInt
                || action.errorCode > kMaximumQuicVarInt
                || !action.payload.empty() || action.flowCredit != 0) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/3 RESET coordinate action is invalid");
            }
            const auto found = m_streams.find(action.streamId);
            StreamState state = found == m_streams.end()
                ? StreamState{}
                : found->second;
            if (state.finalKnown || finalSize < state.nextOffset
                || finalSize > kMaximumQuicVarInt) {
                return LikesProgram::Status::InvalidArgument(
                    u"HTTP/3 RESET final size is invalid");
            }

            const QuicResetStreamFrame frame{
                action.streamId, action.errorCode, finalSize, 0 };
            const auto encoded = LikesProgram::Quic::BuildQuicResetStreamFrame(frame);
            if (!encoded.IsOk()) return encoded.GetStatus();
            const auto parsed = LikesProgram::Quic::ParseQuicResetStreamFrame(
                encoded.Value().data(), encoded.Value().size());
            if (!parsed.IsOk() || parsed.Value().streamId != action.streamId
                || parsed.Value().applicationErrorCode != action.errorCode
                || parsed.Value().finalSize != finalSize) {
                return LikesProgram::Status::Internal(
                    u"QUIC RESET coordinate round trip changed the action");
            }

            state.nextOffset = finalSize;
            state.finalKnown = true;
            state.finalSize = finalSize;
            m_records.push_back({ action.actionId, encoded.Value() });
            m_streams.insert_or_assign(action.streamId, state);
            return encoded.Value();
        }

        const std::vector<WireRecord>& Records() const noexcept {
            return m_records;
        }

        std::uint64_t NextOffset(std::uint64_t streamId) const noexcept {
            const auto state = m_streams.find(streamId);
            return state == m_streams.end() ? 0 : state->second.nextOffset;
        }

        bool FinalKnown(std::uint64_t streamId) const noexcept {
            const auto state = m_streams.find(streamId);
            return state != m_streams.end() && state->second.finalKnown;
        }

    private:
        struct StreamState {
            std::uint64_t nextOffset = 0;
            std::uint64_t finalSize = 0;
            bool finalKnown = false;
        };

        std::unordered_map<std::uint64_t, StreamState> m_streams;
        std::vector<WireRecord> m_records;
    };

    bool IsInvalid(const LikesProgram::Result<std::vector<std::uint8_t>>& value) {
        return !value.IsOk()
            && value.GetStatus().Code() == LikesProgram::StatusCode::InvalidArgument;
    }

    bool IsEqual(const std::vector<std::uint8_t>& left,
        const std::vector<std::uint8_t>& right) {
        return left == right;
    }
}

int main() {
    using LikesProgram::Http::Http3ErrorCode;
    using LikesProgram::Http::Http3QuicEventKind;
    using LikesProgram::Http::Http3QuicTransportFeedbackKind;

    ActionRecorder sink;
    Http3QuicAdapter adapter(&sink);
    CallerWireCoordinateMapper mapper;
    if (!adapter.Feed({ Http3QuicEventKind::HandshakeComplete }).IsOk()) {
        return 1;
    }

    const std::vector<std::uint8_t> firstPayload{ 0xAA, 0xBB };
    if (!adapter.SendStreamData(4, firstPayload).IsOk()
        || sink.actions.size() != 1) {
        return 2;
    }
    const auto firstAction = sink.actions.back();
    const auto firstWire = mapper.MapStreamAction(firstAction, 0, false);
    if (!firstWire.IsOk()
        || !IsEqual(firstWire.Value(), {
            0x0E, 0x04, 0x00, 0x02, 0xAA, 0xBB })
        || mapper.Records().back().actionId != firstAction.actionId
        || !adapter.FeedTransportFeedback({
            Http3QuicTransportFeedbackKind::ActionAccepted,
            firstAction.actionId, firstPayload.size(), 0 }).IsOk()) {
        return 3;
    }

    if (!adapter.SendStreamFin(4).IsOk() || sink.actions.size() != 2) {
        return 4;
    }
    const auto finAction = sink.actions.back();
    const auto finWire = mapper.MapStreamAction(finAction, 2, true);
    if (!finWire.IsOk()
        || !mapper.FinalKnown(4)
        || mapper.NextOffset(4) != 2
        || !adapter.FeedTransportFeedback({
            Http3QuicTransportFeedbackKind::ActionAccepted,
            finAction.actionId, 0, 0 }).IsOk()) {
        return 5;
    }
    const auto wrongAfterFin = mapper.MapStreamAction(finAction, 2, true);
    if (!IsInvalid(wrongAfterFin)) return 6;

    const std::vector<std::uint8_t> secondPayload{ 0x10, 0x11, 0x12 };
    if (!adapter.SendStreamData(8, secondPayload).IsOk()
        || sink.actions.size() != 3) {
        return 7;
    }
    const auto dataAction = sink.actions.back();
    const auto priorRecordCount = mapper.Records().size();
    if (!IsInvalid(mapper.MapStreamAction(dataAction, 1, false))
        || mapper.NextOffset(8) != 0
        || mapper.Records().size() != priorRecordCount) {
        return 8;
    }
    if (!mapper.MapStreamAction(dataAction, 0, false).IsOk()
        || !adapter.FeedTransportFeedback({
            Http3QuicTransportFeedbackKind::ActionAccepted,
            dataAction.actionId, secondPayload.size(), 0 }).IsOk()
        || mapper.NextOffset(8) != secondPayload.size()) {
        return 9;
    }

    if (!adapter.ResetStream(8, static_cast<std::uint64_t>(
            Http3ErrorCode::MessageError)).IsOk()
        || sink.actions.size() != 4) {
        return 10;
    }
    const auto resetAction = sink.actions.back();
    const auto resetRecordCount = mapper.Records().size();
    if (!IsInvalid(mapper.MapResetAction(resetAction, 2))
        || mapper.FinalKnown(8)
        || mapper.Records().size() != resetRecordCount) {
        return 11;
    }
    const auto resetWire = mapper.MapResetAction(resetAction, 3);
    if (!resetWire.IsOk()
        || !mapper.FinalKnown(8)
        || mapper.NextOffset(8) != 3
        || !adapter.FeedTransportFeedback({
            Http3QuicTransportFeedbackKind::ActionAccepted,
            resetAction.actionId, 0, 0 }).IsOk()) {
        return 12;
    }

    Http3QuicAction invalid = dataAction;
    invalid.actionId = 0;
    if (!IsInvalid(mapper.MapStreamAction(invalid, 3, false))) return 13;
    invalid = resetAction;
    invalid.errorCode = kMaximumQuicVarInt + 1;
    if (!IsInvalid(mapper.MapResetAction(invalid, 3))) return 14;
    if (sink.failed || mapper.Records().size() != 4) return 15;

    std::cout << "passed=true"
              << " data_records=3"
              << " reset_records=1"
              << " action_id_external=true"
              << " final_size_stream8=" << mapper.NextOffset(8)
              << '\n';
    return 0;
}
