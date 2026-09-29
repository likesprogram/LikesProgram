#include <LikesProgram/Http/Http.hpp>

#include <iostream>

namespace {
    class ExampleTransport final : public LikesProgram::Http::HttpTransport {
    public:
        // 示例传输只返回本地响应，真实应用可在此组合任意网络实现。
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Exchange(
            const LikesProgram::Http::HttpRequest& request,
            LikesProgram::Http::HttpVersion version) override {
            LikesProgram::Http::HttpResponse response;
            response.statusCode = 200;
            response.reason = "OK";
            response.headers.push_back({ "X-Protocol", LikesProgram::Http::HttpVersionName(version) });
            response.body.assign(request.target.begin(), request.target.end());
            return response;
        }
    };

    class ExampleHandler final : public LikesProgram::Http::HttpRequestHandler {
    public:
        // 示例处理器只负责业务响应，不负责网络监听。
        LikesProgram::Result<LikesProgram::Http::HttpResponse> Handle(
            const LikesProgram::Http::HttpRequest& request) override {
            LikesProgram::Http::HttpResponse response;
            response.statusCode = 200;
            response.reason = "OK";
            response.body.assign(request.target.begin(), request.target.end());
            return response;
        }
    };
}

int main() {
    LikesProgram::Http::HttpRequest request;                 // 示例请求报文
    request.method = "GET";
    request.target = "/health";
    request.headers.push_back({ "Host", "localhost" });

    const auto requestBytes = LikesProgram::Http::BuildHttp1Request(request);
    if (!requestBytes.IsOk()) {
        std::cerr << requestBytes.GetStatus().ToString().ToStdString() << '\n';
        return 1;
    }

    const auto parsedRequest = LikesProgram::Http::ParseHttp1Request(requestBytes.Value());
    if (!parsedRequest.IsOk()) {
        std::cerr << parsedRequest.GetStatus().ToString().ToStdString() << '\n';
        return 2;
    }

    LikesProgram::Http::Http2Frame settings;                 // HTTP/2 空 SETTINGS 帧
    settings.type = static_cast<std::uint8_t>(LikesProgram::Http::Http2FrameType::Settings);
    settings.streamId = 0;

    const auto frameBytes = LikesProgram::Http::BuildHttp2Frame(settings);
    if (!frameBytes.IsOk()) {
        std::cerr << frameBytes.GetStatus().ToString().ToStdString() << '\n';
        return 3;
    }

    LikesProgram::Http::Http3Frame data;                    // HTTP/3 DATA 帧
    data.type = static_cast<std::uint64_t>(LikesProgram::Http::Http3FrameType::Data);
    data.payload = { 'h', '3' };
    const auto http3Bytes = LikesProgram::Http::BuildHttp3Frame(data);
    if (!http3Bytes.IsOk()) {
        std::cerr << http3Bytes.GetStatus().ToString().ToStdString() << '\n';
        return 4;
    }

    ExampleTransport transport;
    LikesProgram::Http::HttpSession client(&transport);
    const auto response = client.Send(request);
    if (!response.IsOk()) {
        std::cerr << response.GetStatus().ToString().ToStdString() << '\n';
        return 5;
    }

    ExampleHandler handler;
    LikesProgram::Http::HttpSession server(nullptr, &handler);
    const auto handled = server.Handle(request);
    if (!handled.IsOk()) {
        std::cerr << handled.GetStatus().ToString().ToStdString() << '\n';
        return 6;
    }

    std::cout << LikesProgram::Http::PackageName()
        << " parsed " << parsedRequest.Value().method
        << " " << parsedRequest.Value().target
        << ", built HTTP/2 " << LikesProgram::Http::Http2FrameTypeName(settings.type)
        << " frame bytes=" << frameBytes.Value().size()
        << ", built HTTP/3 " << LikesProgram::Http::Http3FrameTypeName(data.type)
        << " frame bytes=" << http3Bytes.Value().size()
        << ", session status=" << response.Value().statusCode
        << ", handled bytes=" << handled.Value().body.size() << '\n';
    return 0;
}
