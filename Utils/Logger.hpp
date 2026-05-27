#pragma once

#include <spdlog/spdlog.h>
#include <sstream>

namespace Utils {

class LogStream {
public:
    explicit LogStream(spdlog::level::level_enum lvl,
                       spdlog::source_loc loc = {})
        : level_(lvl), loc_(loc) {}

    ~LogStream() {
        spdlog::default_logger_raw()->log(loc_, level_, oss_.str());
    }

    template<typename T>
    std::ostringstream& operator<<(const T& msg) {
        oss_ << msg;
        return oss_;
    }

private:
    std::ostringstream oss_;
    spdlog::level::level_enum level_;
    spdlog::source_loc loc_;
};

void InitLogger();

} // namespace Utils

#define LOG_DEBUG Utils::LogStream(spdlog::level::debug, spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION})
#define LOG_INFO  Utils::LogStream(spdlog::level::info, spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION})
#define LOG_WARN  Utils::LogStream(spdlog::level::warn, spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION})
#define LOG_ERROR Utils::LogStream(spdlog::level::err, spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION})