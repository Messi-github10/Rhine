#include "Logger.hpp"

namespace Utils {

void InitLogger() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%# %!] %v");
    spdlog::set_level(spdlog::level::debug);
}

} // namespace Utils