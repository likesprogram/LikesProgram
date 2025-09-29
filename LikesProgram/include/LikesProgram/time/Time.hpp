#pragma once
#include "../String.hpp"
#include <chrono>

namespace LikesProgram {
	namespace Time {
        using TimePoint = std::chrono::system_clock::time_point;
        using Duration = std::chrono::nanoseconds;
        using Nanoseconds = std::chrono::nanoseconds;
        using Microseconds = std::chrono::microseconds;
        using Milliseconds = std::chrono::milliseconds;
        using Seconds = std::chrono::seconds;
        using Minutes = std::chrono::minutes;
        using Hours = std::chrono::hours;
        using Days = std::chrono::duration<int64_t, std::ratio<86400>>;

        // ��ʽ��ʱ��
		LikesProgram::String FormatTime(const TimePoint& tp, const LikesProgram::String& fmt = u"%Y-%m-%d %H:%M:%S");
        
        namespace Convert {
            // ���� �� system_clock::time_point
            inline TimePoint NsToSystemClock(Nanoseconds ns) {
                return TimePoint{ std::chrono::duration_cast<TimePoint::duration>(ns) };
            }

            // ���� �� system_clock::time_point
            inline TimePoint NsToSystemClock(int64_t ns) {
                return NsToSystemClock(Nanoseconds{ ns });
            }

            // system_clock::time_point �� ����
            inline Nanoseconds SystemClockToDuration(const TimePoint& tp) {
                return std::chrono::duration_cast<Nanoseconds>(tp.time_since_epoch());
            }

            // ���� �� ΢��
            inline double NsToUs(int64_t ns) { return static_cast<double>(ns) / 1'000.0; }
            // ���� �� ����
            inline double NsToMs(int64_t ns) { return static_cast<double>(ns) / 1'000'000.0; }
            // ���� �� ��
            inline double NsToS(int64_t ns) { return static_cast<double>(ns) / 1'000'000'000.0; }
            // ���� �� ����
            inline double NsToMin(int64_t ns) { return static_cast<double>(ns) / 60'000'000'000.0; }
            // ���� �� Сʱ
            inline double NsToH(int64_t ns) { return static_cast<double>(ns) / 3'600'000'000'000.0; }

            // ΢�� �� ���루���������ע�ⷶΧ��
            inline int64_t UsToNs(double us) { return static_cast<int64_t>(us * 1'000.0); }
            // ΢�� �� ���루���������ע�ⷶΧ��
            inline double UsToMs(double us) { return us / 1'000.0; }
            // ΢�� �� ��
            inline double UsToS(double us) { return us / 1'000'000.0; }
            // ΢�� �� ����
            inline double UsToMin(double us) { return us / 60'000'000.0; }
            // ΢�� �� Сʱ
            inline double UsToH(double us) { return us / 3'600'000'000.0; }

            // ���� �� ���루���������ע�ⷶΧ��
            inline int64_t MsToNs(double ms) { return static_cast<int64_t>(ms * 1'000'000.0); }
            // ���� �� ΢�루���������ע�ⷶΧ��
            inline double MsToUs(double ms) { return ms * 1'000.0; }
            // ���� �� ��
            inline double MsToS(double ms) { return ms / 1'000.0; }
            // ���� �� ����
            inline double MsToMin(double ms) { return ms / 60'000.0; }
            // ���� �� Сʱ
            inline double MsToH(double ms) { return ms / 3'600'000.0; }

            // �� �� ���루���������ע�ⷶΧ��
            inline int64_t SToNs(double s) { return static_cast<int64_t>(s * 1'000'000'000.0); }
            // �� �� ΢�루���������ע�ⷶΧ��
            inline double SToUs(double s) { return s * 1'000'000.0; }
            // �� �� ���루���������ע�ⷶΧ��
            inline double SToMs(double s) { return s * 1'000.0; }
            // �� �� ����
            inline double SToMin(double s) { return s / 60.0; }
            // �� �� Сʱ
            inline double SToH(double s) { return s / 3'600.0; }

            // ���� �� ���루���������ע�ⷶΧ��
            inline int64_t MinToNs(double min) { return static_cast<int64_t>(min * 60.0 * 1'000'000'000.0); }
            // ���� �� ΢�루���������ע�ⷶΧ��
            inline double MinToUs(double min) { return min * 60.0 * 1'000'000.0; }
            // ���� �� ���루���������ע�ⷶΧ��
            inline double MinToMs(double min) { return min * 60.0 * 1'000.0; }
            // ���� �� �루���������ע�ⷶΧ��
            inline double MinToS(double min) { return min * 60.0; }
            // ���� �� Сʱ
            inline double MinToH(double min) { return min / 60.0; }

            // Сʱ �� ���루���������ע�ⷶΧ��
            inline int64_t HToNs(double h) { return static_cast<int64_t>(h * 3'600.0 * 1'000'000'000.0); }
            // Сʱ �� ΢�루���������ע�ⷶΧ��
            inline double HToUs(double h) { return h * 3'600.0 * 1'000'000.0; }
            // Сʱ �� ���루���������ע�ⷶΧ��
            inline double HToMs(double h) { return h * 3'600.0 * 1'000.0; }
            // Сʱ �� �루���������ע�ⷶΧ��
            inline double HToS(double h) { return h * 3'600.0; }
            // Сʱ �� ���ӣ����������ע�ⷶΧ��
            inline double HToMin(double h) { return h * 60.0; }
        }
	}
}