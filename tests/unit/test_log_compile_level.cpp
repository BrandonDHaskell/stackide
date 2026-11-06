// Built with only Error and Warn surviving compilation, whatever the build type.
#undef STACKIDE_LOG_COMPILE_LEVEL
#define STACKIDE_LOG_COMPILE_LEVEL 2

#include <stackide/base/log.hpp>

#include <catch2/catch_test_macros.hpp>

using stackide::LogCategory;

TEST_CASE("levels above the compile level are removed even when enabled at runtime")
{
    stackide::LogConfig config;
    config.to_stderr = false;
    config.level = stackide::LogLevel::Trace;
    const stackide::LogSession session{config};
    int evaluations = 0;
    const auto argument = [&] { return ++evaluations; };

    STACKIDE_LOG_INFO(LogCategory::General, "{}", argument());
    STACKIDE_LOG_TRACE(LogCategory::General, "{}", argument());
    CHECK(evaluations == 0);

    STACKIDE_LOG_WARN(LogCategory::General, "{}", argument());
    CHECK(evaluations == 1);
}
