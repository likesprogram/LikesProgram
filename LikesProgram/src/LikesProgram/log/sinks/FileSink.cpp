#include "../../../../include/LikesProgram/log/sinks/FileSink.hpp"
#include "../../../../include/LikesProgram/time/Time.hpp"
#include <fstream>
#include <filesystem>

namespace LikesProgram {
	namespace Log {
		class FileSink::FileSinkImpl {
		public:
			explicit FileSinkImpl(const LikesProgram::String& path, const LikesProgram::String& filename, size_t maxFileSizeMB = 30)
				: m_path(path), m_filename(filename), m_current_size(0), m_max_file_size(maxFileSizeMB * 1024 * 1024) {
				if (m_path.Empty()) m_path = u"./logs"; // 默认路径
				if (m_filename.Empty()) m_filename = u"Logger.log"; // 默认文件名
				OpenNewFile();
			}

			void Write(const String& formatted, String::Encoding encoding, std::chrono::system_clock::time_point timestamp) {
				if (NeedRotate(timestamp)) OpenNewFile(); // 切分新文件
				if (!m_file.is_open()) return; // 打开失败，丢弃本次输出

				// 执行记录
				m_file << formatted.ToStdString(encoding) << std::endl;
				m_current_size += formatted.Size();
			}
		private:
			// 打开新日志文件
			void OpenNewFile() {
				if (m_file.is_open()) m_file.close();

				// 生成新文件名
				m_last_rotate_time = std::chrono::system_clock::now();
				LikesProgram::String timeDir = LikesProgram::String::Format(u"{:tYYYY-MM-DD}", m_last_rotate_time);

				// 确保目录存在（简单实现，实际应递归创建）
				std::filesystem::path dir = std::filesystem::path(m_path.ToStdString()) / timeDir.ToStdString();
				std::filesystem::create_directories(dir); // 创建文件夹

				std::string file;
				while (true)
				{
					file = BuildLogFileName(dir, m_filename.ToStdString(), m_file_index);

					// 文件不存在，直接使用
					if (!std::filesystem::exists(file)) break;

					auto size = std::filesystem::file_size(file);

					if (m_max_file_size == 0 || size < m_max_file_size) break;

					// 已满，尝试下一个
					++m_file_index;
				}

				// 打开文件
				m_file.open(file, std::ios::out | std::ios::app);
				if (!m_file) throw std::runtime_error("Failed to open log file: " + file);
			}

			// 是否需要切换文件
			bool NeedRotate(std::chrono::system_clock::time_point time) {
				if (!m_file.is_open()) return false;

				// 按文件大小轮转
				if (m_max_file_size > 0 && m_current_size >= m_max_file_size) return true;

				// 按日期轮转
				std::time_t now_t = std::chrono::system_clock::to_time_t(time);
				std::time_t last_t = std::chrono::system_clock::to_time_t(m_last_rotate_time);
				std::tm now_tm = LikesProgram::Time::ToLocalTime(now_t);
				std::tm last_tm = LikesProgram::Time::ToLocalTime(last_t);
				if (now_tm.tm_year != last_tm.tm_year || now_tm.tm_mon != last_tm.tm_mon || now_tm.tm_mday != last_tm.tm_mday) {
					m_file_index = 0;
					return true;
				}

				return false;
			}

			// 构建日志文件名（包含索引）
			std::string BuildLogFileName(const std::filesystem::path& dir, const std::string& base, size_t index) {
				if (index == 0) return (dir / base).string();
				return (dir / (std::to_string(index) + "_" + base)).string();
			}
		private:
			std::ofstream m_file;
			LikesProgram::String m_path; // 文件路径
			size_t m_file_index = 0; // 文件索引（同一日期多个文件时使用）
			LikesProgram::String m_filename; // 当前文件名
			size_t m_current_size = 0; // 当前文件大小
			size_t m_max_file_size = 0; // 最大文件大小
			std::chrono::system_clock::time_point m_last_rotate_time; // 上次切换文件的时间
		};

		FileSink::FileSink(const LikesProgram::String& path, const LikesProgram::String& filename, size_t maxFileSizeMB)
		: Sink(u"FileSink") {
			m_impl = new FileSinkImpl(path, filename, maxFileSizeMB);
		}
		FileSink::~FileSink() {
			if(m_impl) delete m_impl;
			m_impl = nullptr;
		}

		void FileSink::Write(const Message& message) {
			LikesProgram::String formatted = FormatLogMessage(message);
			m_impl->Write(formatted, message.encoding, message.timestamp);
		}

		std::shared_ptr<Sink> FileSink::CreateSink(const LikesProgram::String& path, const LikesProgram::String& filename, size_t maxFileSizeMB) {
			return std::make_shared<FileSink>(path, filename, maxFileSizeMB);
		}
	}
}