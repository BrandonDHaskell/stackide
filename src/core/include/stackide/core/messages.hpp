#pragma once

// The only types that cross the UI/core boundary. See docs/adr/0001-ui-core-split.md:
// values only, so that these can later be sent over a network.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace stackide
{

    // Identifies one long-running task across its progress messages.
    struct ProgressToken
    {
        std::uint64_t value = 0;

        auto operator<=>(const ProgressToken&) const = default;
    };

    // The progress messages mirror LSP's $/progress, so that language servers can
    // plug in directly in Phase 2.
    struct ProgressBegin
    {
        ProgressToken token;
        std::string title;
        bool cancellable = false;
        std::optional<std::uint8_t> percentage; // 0 to 100, empty if indeterminate

        bool operator==(const ProgressBegin&) const = default;
    };

    struct ProgressReport
    {
        ProgressToken token;
        std::string message;
        std::optional<std::uint8_t> percentage; // 0 to 100, empty if indeterminate

        bool operator==(const ProgressReport&) const = default;
    };

    struct ProgressEnd
    {
        ProgressToken token;
        std::string message;

        bool operator==(const ProgressEnd&) const = default;
    };

    // Core state changed in a way that needs a new frame.
    struct RedrawRequested
    {
        bool operator==(const RedrawRequested&) const = default;
    };

    struct CancelProgress
    {
        ProgressToken token;

        bool operator==(const CancelProgress&) const = default;
    };

    struct ShutdownRequested
    {
        bool operator==(const ShutdownRequested&) const = default;
    };

    using CoreToUi = std::variant<ProgressBegin, ProgressReport, ProgressEnd, RedrawRequested>;
    using UiToCore = std::variant<CancelProgress, ShutdownRequested>;

} // namespace stackide
