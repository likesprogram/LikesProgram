#pragma once
#include "../LikesProgram/Configuration.hpp"
#include "../LikesProgram/String.hpp"
#include <iostream>
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
        proj1[u"stars"] = 800;
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
            std::cout << "User ID: " << userId
                << ", Street: " << street
                << ", Zip: " << zip
                << ", Active: " << active << std::endl;
        }
        catch (std::exception& e) {
            std::cerr << "Conversion error: " << e.what() << std::endl;
        }

        // CastPolicy 示例
        try {
            double userIdDouble = cfg[u"user_id"].AsDouble(LikesProgram::Configuration::CastPolicy::Strict);
            std::cout << "User ID as double: " << userIdDouble << std::endl;
        }
        catch (std::exception& e) {
            std::cerr << "Strict cast error: " << e.what() << std::endl;
        }

        // 安全获取 tryGet
        int stars = 0;
        if (cfg[u"projects"][u"list"][1][u"stars"].TryGet(stars)) {
            std::cout << "Project 2 stars: " << stars << std::endl;
        }

        // 遍历对象
        std::cout << "\nUser info:\n";
        for (auto it = cfg.beginObject(); it != cfg.endObject(); ++it) {
#ifdef _WIN32
            std::cout << it->first << ": " << it->second.AsString().ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
            std::cout << it->first << ": " << it->second.AsString() << std::endl;
#endif
        }

        // 遍历数组
        std::cout << "\nHobbies:\n";
        for (const auto& hobby : cfg[u"hobbies"]) {
#ifdef _WIN32
            std::cout << "- " << hobby.AsString().ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
            std::cout << "- " << hobby.AsString() << std::endl;
#endif
        }
        // 遍历数组
        std::cout << "\nHobbies:\n";
        for (size_t i = 0; i < cfg[u"hobbies"].Size(); ++i) {
#ifdef _WIN32  // 使用 at() 确保安全访问
            std::cout << "- " << cfg[u"hobbies"].At(i).AsString().ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
            std::cout << "- " << cfg[u"hobbies"].At(i).AsString() << std::endl;
#endif
        }

        // 遍历嵌套数组 + 对象
        std::cout << "\nProjects:\n";
        for (const auto& project : cfg[u"projects"][u"list"]) {
#ifdef _WIN32
            std::cout << "- " << project[u"name"].AsString().ToStdString(LikesProgram::String::Encoding::GBK)
#else 
            std::cout << "- " << project[u"name"].AsString()
#endif
                << " (" << project[u"stars"].AsInt() << " stars)" << std::endl;
        }
        // JSON 序列化 / 反序列化

        // 设置默认的序列化器，通过该函数设置的序列化器，所有 Configuration 对象都可以使用，除非 你设置了 Configuration::SetSerializer
        // 默认的序列化器就是 JsonSerializer 除非你设置了其他序列化器，或是你需要自定义一个序列化器
        //LikesProgram::Configuration::SetDefaultSerializer(LikesProgram::Configuration::CreateJsonSerializer());

        // 使用默认的序列化器，获取 JSON 文本
        LikesProgram::String jsonText = cfg.Dump(4); // 缩进4空格
#ifdef _WIN32
        std::cout << "\nSerialized JSON:\n" << jsonText.ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
        std::cout << "\nSerialized JSON:\n" << jsonText << std::endl;
#endif
        // 使用默认的序列化器，反序列化
        LikesProgram::Configuration loadedCfg;
        loadedCfg.Load(jsonText);
        std::cout << "\nLoaded project 1 name: "
#ifdef _WIN32
            << loadedCfg[u"projects"][u"list"][0][u"name"].AsString().ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
            << loadedCfg[u"projects"][u"list"][0][u"name"].AsString() << std::endl;
#endif

        std::cout << std::endl;
        // 异常捕获示例
        try {
            int invalid = loadedCfg[u"nonexistent"].AsInt(); // key 不存在
        }
        catch (std::exception& e) {
            std::cerr << "Expected error (key missing): " << e.what() << std::endl;
        }

        try {
            double invalidType = loadedCfg[u"user_name"].AsDouble(); // 类型不匹配
        }
        catch (std::exception& e) {
            std::cerr << "Expected error (type mismatch): " << e.what() << std::endl;
        }

        try {
            auto hobby = loadedCfg[u"hobbies"].At(10); // 越界
        }
        catch (std::exception& e) {
            std::cerr << "Expected error (array out-of-range): " << e.what() << std::endl;
        }

        std::cout << std::endl;
        // 使用自定义序列化器，注意：因为自定义序列化器是JSON，与上面使用的默认序列化器输出格式相同，因此才可以正常读取
        // 在正常开发中，需要注意：不同序列化器读取的配置文件不同
        // 在实际使用中，推荐使用 SetDefaultSerializer，不推荐给对象单独设置序列化器
        // 因为这样会造成输出格式紊乱，导致输出结果不可读
        cfg.SetSerializer(LikesProgram::CreateSimpleSerializer());
        LikesProgram::String simpleText1 = cfg.Dump(4); // 缩进4空格
#ifdef _WIN32
        std::cout << "\SimpleSerializer Text:\n" << simpleText1.ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
        std::cout << "\nSimpleSerializer Text:\n" << simpleText1 << std::endl;
#endif
        LikesProgram::Configuration loadedCfg1;
        loadedCfg1.SetSerializer(LikesProgram::CreateSimpleSerializer());
        loadedCfg1.Load(simpleText1);
        std::cout << "\nLoaded project 1 name: "
#ifdef _WIN32
            << loadedCfg1[u"projects"][u"list"][0][u"name"].AsString().ToStdString(LikesProgram::String::Encoding::GBK) << std::endl;
#else
            << loadedCfg1[u"projects"][u"list"][0][u"name"].AsString() << std::endl;
#endif
	}
}