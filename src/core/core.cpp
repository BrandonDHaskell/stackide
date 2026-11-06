#include <stackide/core/core.hpp>

#include <stackide/base/log.hpp>

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace stackide
{
    namespace
    {

        constexpr std::uint8_t kMaxPercentage = 100;
        constexpr std::size_t kToCoreCapacity = 256;

    } // namespace

    void ProgressReporter::report(std::string message, std::optional<std::uint8_t> percentage)
    {
        if (percentage)
        {
            percentage = std::min(*percentage, kMaxPercentage);
        }
        to_ui_->push(ProgressReport{.token = token_, .message = std::move(message), .percentage = percentage},
                     shutdown_);
    }

    Core::Core(std::function<void()> on_wake, const CoreConfig& config) :
        to_ui_(config.message_capacity, std::move(on_wake)), to_core_(kToCoreCapacity), workers_(config.workers),
        message_thread_([this](const std::stop_token& stop) { run_messages(stop); })
    {
    }

    Core::~Core()
    {
        // Releases any task blocked on a full queue before the pool joins it.
        shutdown_.request_stop();
    }

    bool Core::send(const UiToCore& message) { return to_core_.try_push(UiToCore{message}); }

    std::optional<ProgressToken> Core::start_progress(Lane lane, std::string title, bool cancellable, ProgressTask task)
    {
        const std::unique_lock lock{tasks_mutex_, std::try_to_lock};
        if (!lock.owns_lock())
        {
            return std::nullopt;
        }

        const ProgressToken token{.value = next_token_};
        auto handle = workers_.scheduler(lane).spawn(
            [this, token, title = std::move(title), cancellable, task = std::move(task)](const std::stop_token& stop)
            {
                const std::stop_token shutdown = shutdown_.get_token();
                to_ui_.push(
                    ProgressBegin{
                        .token = token,
                        .title = title,
                        .cancellable = cancellable,
                        .percentage = std::nullopt,
                    },
                    shutdown);

                std::string outcome;
                ProgressReporter reporter{to_ui_, token, shutdown};
                try
                {
                    task(stop, reporter);
                    outcome = stop.stop_requested() ? "cancelled" : "";
                }
                catch (const std::exception& error)
                {
                    STACKIDE_LOG_ERROR(LogCategory::Core, "task '{}' failed: {}", title, error.what());
                    outcome = "failed";
                }
                catch (...)
                {
                    STACKIDE_LOG_ERROR(LogCategory::Core, "task '{}' failed with an unknown exception", title);
                    outcome = "failed";
                }

                forget_task(token);
                to_ui_.push(ProgressEnd{.token = token, .message = std::move(outcome)}, shutdown);
            });
        if (!handle)
        {
            return std::nullopt;
        }

        ++next_token_;
        if (cancellable)
        {
            cancellable_tasks_.insert_or_assign(token.value, std::move(handle));
        }
        return token;
    }

    void Core::forget_task(ProgressToken token)
    {
        const std::scoped_lock lock{tasks_mutex_};
        cancellable_tasks_.erase(token.value);
    }

    void Core::run_messages(const std::stop_token& stop)
    {
        while (std::optional<UiToCore> message = to_core_.pop(stop))
        {
            handle(*message);
        }
    }

    void Core::handle(const UiToCore& message)
    {
        std::visit(
            [this](const auto& value)
            {
                using Message = std::decay_t<decltype(value)>;
                const std::scoped_lock lock{tasks_mutex_};
                if constexpr (std::is_same_v<Message, CancelProgress>)
                {
                    // Finding nothing is normal: the task may have just finished.
                    if (const auto found = cancellable_tasks_.find(value.token.value);
                        found != cancellable_tasks_.end())
                    {
                        found->second->request_stop();
                    }
                }
                else
                {
                    for (auto& [token, task] : cancellable_tasks_)
                    {
                        task->request_stop();
                    }
                }
            },
            message);
    }

} // namespace stackide
