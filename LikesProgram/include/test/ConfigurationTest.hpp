#pragma once
#include "../LikesProgram/Configuration.hpp"
#include "../LikesProgram/String.hpp"
#include "../LikesProgram/Logger.hpp"
#include <sstream>
#include <vector>

namespace LikesProgram {

    /*
     * SimpleSerializer 示例说明
     *
     * 在本示例中，我们实现了一个简单的 Serializer，序列化成类似：
     * user_name=Alice
     * user_id=987654321012345
     * address.street=456 Park Ave
     * hobbies.0=reading
     *
     */

    class SimpleSerializer : public Configuration::Serializer {
    public:
        String Serialize(const Configuration& cfg, int indent = 2) const override {
            std::ostringstream oss;
            SerializeRec(cfg, "", oss);
            return String(oss.str().c_str());
        }

        Configuration Deserialize(const String& text) const override {
            Configuration cfg;
            std::istringstream iss(text.ToStdString());
            std::string line;
            while (std::getline(iss, line)) {
                auto eq_pos = line.find('=');
                if (eq_pos == std::string::npos) continue; // 忽略不合法行
                std::string path = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);
                SetValueByPath(cfg, path, value);
            }
            return cfg;
        }

    private:
        void SerializeRec(const Configuration& cfg, const std::string& prefix, std::ostringstream& oss) const {
            if (cfg.IsObject()) {
                for (auto& [k, v] : cfg.AsObject()) {
                    std::string newPrefix = prefix.empty() ? k.ToStdString() : prefix + "." + k.ToStdString();
                    SerializeRec(v, newPrefix, oss);
                }
            }
            else if (cfg.IsArray()) {
                const auto& arr = cfg.AsArray();
                for (size_t i = 0; i < arr.size(); ++i) {
                    std::string newPrefix = prefix + "." + std::to_string(i);
                    SerializeRec(arr[i], newPrefix, oss);
                }
            }
            else { // 基本类型
                std::string val;
                if (cfg.IsBool()) val = cfg.AsBool() ? "true" : "false";
                else if (cfg.IsInt()) val = std::to_string(cfg.AsInt());
                else if (cfg.IsInt64()) val = std::to_string(cfg.AsInt64());
                else if (cfg.IsDouble()) val = std::to_string(cfg.AsDouble());
                else if (cfg.IsString()) val = cfg.AsString().ToStdString();
                else val = "";
                oss << prefix << "=" << val << "\n";
            }
        }

        void SetValueByPath(Configuration& cfg, const std::string& path, const std::string& value) const {
            size_t dotPos = path.find('.');
            if (dotPos == std::string::npos) {
                // 最底层 key
                cfg[String(path)] = value.c_str();
            }
            else {
                std::string head = path.substr(0, dotPos);
                std::string tail = path.substr(dotPos + 1);
                // 判断是否数组索引
                bool isArray = !tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) { return std::isdigit(c) || c == '.'; });
                if (std::isdigit(tail[0])) { // 数组
                    int idx = std::stoi(tail.substr(0, tail.find('.')));
                    Configuration& arr = cfg[String(head)];
                    while (arr.Size() <= idx) arr.Push_back(Configuration());
                    SetValueByPath(arr[idx], tail.substr(tail.find('.') + 1), value);
                }
                else { // 对象
                    SetValueByPath(cfg[String(head)], tail, value);
                }
            }
        }
    };

    inline std::shared_ptr<Configuration::Serializer> CreateSimpleSerializer() {
        return std::make_shared<SimpleSerializer>();
    }
}


namespace ConfigurationTest {
	void Test() {
#ifdef _DEBUG
        auto& logger = LikesProgram::Logger::Instance(true);
#else
        auto& logger = LikesProgram::Logger::Instance();
#endif

#ifdef _WIN32
        logger.SetEncoding(LikesProgram::String::Encoding::GBK);
#endif
        logger.SetLevel(LikesProgram::Logger::LogLevel::Trace);
        // 内置控制台输出 Sink
        logger.AddSink(LikesProgram::Logger::CreateConsoleSink()); // 输出到控制台

        // 创建顶层配置对象
        LikesProgram::Configuration cfg;

        // 对象直接赋值
        cfg[u"user_name"] = u"Alice";
        cfg[u"user_id"] = int64_t(987654321012345);

        // 使用 emplace 添加键值
        cfg.Emplace(u"is_active", true);

        // 嵌套对象
        LikesProgram::Configuration address;
        address[u"street"] = u"456 Park Ave";
        address[u"city"] = u"Shanghai";
        address[u"zip"] = 200000;
        cfg[u"address"] = address;

        // 数组操作
        LikesProgram::Configuration::Array hobbies; // 明确指定为数组类型
        hobbies.push_back(u"reading"); // LikesProgram::Configuration::Array 本质上就是 std::vector 因此需要使用 push_back
        hobbies.push_back(u"gaming");
        hobbies.push_back(u"traveling");
        // 直接添加键值
        //cfg["hobbies"] = std::move(hobbies);
        // 使用 emplace 添加键值
        cfg.Emplace(u"hobbies", std::move(hobbies));

        // 多层嵌套对象和数组
        LikesProgram::Configuration projects;
        LikesProgram::Configuration projectList; // 自动识别为数组

        LikesProgram::Configuration proj1;
        proj1[u"name"] = u"LikesProgram - C++ 通用库";
        proj1[u"stars"] = 0;
        projectList.Push_back(proj1); // LikesProgram::Configuration 中需要使用 Push_back 添加数组元素

        LikesProgram::Configuration proj2;
        proj2[u"name"] = u"MyCoolApp";
        proj2[u"stars"] = 1500;
        projectList.Push_back(proj2);

        projects[u"list"] = projectList;
        cfg[u"projects"] = projects;

        // 类型转换示例
        try {
            int zip = cfg[u"address"][u"zip"].AsInt();
            int64_t userId = cfg[u"user_id"].AsInt64();
            LikesProgram::String street = cfg[u"address"][u"street"].AsString();
            bool active = cfg[u"is_active"].AsBool();
            
            LikesProgram::String out = u"User ID: ";
            out += LikesProgram::String(std::to_string(userId));
            out += u", Street: "; out += street;
            out += u", Zip: "; out += LikesProgram::String(std::to_string(zip));
            out += u", Active: "; out += LikesProgram::String(active ? u"true" : u"false");
            LOG_DEBUG(out);
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Conversion error: ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        // CastPolicy 示例
        try {
            double userIdDouble = cfg[u"user_id"].AsDouble(LikesProgram::Configuration::CastPolicy::Strict);
            LikesProgram::String out = u"User ID as double: ";
            out += (LikesProgram::String)std::to_string(userIdDouble);
            LOG_DEBUG(out);
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Strict cast error: ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        // 安全获取 tryGet
        int stars = 0;
        if (cfg[u"projects"][u"list"][1][u"stars"].TryGet(stars)) {
            LikesProgram::String out = u"Project 2 stars: ";
            out += (LikesProgram::String)std::to_string(stars);
            LOG_DEBUG(out);
        }

        // 遍历对象
        LOG_WARN(u"User info : ");
        for (auto it = cfg.beginObject(); it != cfg.endObject(); ++it) {
            LikesProgram::String out = it->first;
            out += u": "; out += it->second.AsString();
            LOG_DEBUG(out);
        }

        // 遍历数组
        LOG_WARN(u"Hobbies : ");
        for (const auto& hobby : cfg[u"hobbies"]) {
            LikesProgram::String out = u"- ";
            out += hobby.AsString();
            LOG_DEBUG(out);
        }
        // 遍历数组
        LOG_WARN(u"Hobbies : ");
        for (size_t i = 0; i < cfg[u"hobbies"].Size(); ++i) {
            LikesProgram::String out = u"- ";
            out += cfg[u"hobbies"].At(i).AsString();
            LOG_DEBUG(out);
        }

        // 遍历嵌套数组 + 对象
        LOG_WARN(u"nProjects : ");
        for (const auto& project : cfg[u"projects"][u"list"]) {
            LikesProgram::String out = u"- ";
            out += project[u"name"].AsString();
            out += u" ("; out += (LikesProgram::String)std::to_string(project[u"stars"].AsInt());
            out += u" stars)";
            LOG_DEBUG(out);
        }
        // JSON 序列化 / 反序列化

        // 设置默认的序列化器，通过该函数设置的序列化器，所有 Configuration 对象都可以使用，除非 你设置了 Configuration::SetSerializer
        // 默认的序列化器就是 JsonSerializer 除非你设置了其他序列化器，或是你需要自定义一个序列化器
        //LikesProgram::Configuration::SetDefaultSerializer(LikesProgram::Configuration::CreateJsonSerializer());

        // 使用默认的序列化器，获取 JSON 文本
        LikesProgram::String jsonText = cfg.Dump(4); // 缩进4空格
        LikesProgram::String jsonTextOut = u"Serialized JSON : \n";
        jsonTextOut += jsonText;
        LOG_DEBUG(jsonTextOut);

        // 使用默认的序列化器，反序列化
        LikesProgram::Configuration loadedCfg;
        loadedCfg.Load(jsonText);
        LikesProgram::String loadedOut = u"Loaded project 1 name: ";
        loadedOut += loadedCfg[u"projects"][u"list"][0][u"name"].AsString();
        LOG_DEBUG(loadedOut);

        // 异常捕获示例
        try {
            int invalid = loadedCfg[u"nonexistent"].AsInt(); // key 不存在
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Expected error (key missing): ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        try {
            double invalidType = loadedCfg[u"user_name"].AsDouble(); // 类型不匹配
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Expected error (type mismatch): ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        try {
            auto hobby = loadedCfg[u"hobbies"].At(10); // 越界
        }
        catch (std::exception& e) {
            LikesProgram::String out = u"Expected error (array out-of-range): ";
            out += (LikesProgram::String)e.what();
            LOG_ERROR(out);
        }

        // 使用自定义序列化器，注意：因为自定义序列化器是JSON，与上面使用的默认序列化器输出格式相同，因此才可以正常读取
        // 在正常开发中，需要注意：不同序列化器读取的配置文件不同
        // 在实际使用中，推荐使用 SetDefaultSerializer，不推荐给对象单独设置序列化器
        // 因为这样会造成输出格式紊乱，导致输出结果不可读
        cfg.SetSerializer(LikesProgram::CreateSimpleSerializer());
        LikesProgram::String simpleText = cfg.Dump(4); // 缩进4空格
        LikesProgram::String simpleTextOut = u"SimpleSerializer Text : \n";
        simpleTextOut += simpleText;
        LOG_DEBUG(simpleTextOut);

        LikesProgram::Configuration loadedCfg1;
        loadedCfg1.SetSerializer(LikesProgram::CreateSimpleSerializer());
        loadedCfg1.Load(simpleText);
        LikesProgram::String loadedOut1 = u"Loaded project 1 name: ";
        loadedOut1 += loadedCfg1[u"projects"][u"list"][0][u"name"].AsString();
        LOG_DEBUG(loadedOut1);

        std::this_thread::sleep_for(std::chrono::seconds(1)); // 给后台线程一点时间输出
        logger.Shutdown();
	}
}