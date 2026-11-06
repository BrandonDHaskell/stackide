#include <stackide/core/messages.hpp>

#include <concepts>
#include <type_traits>

namespace stackide
{
    namespace
    {

        // Checked per alternative, because a variant's own copyability wouldn't
        // reject a message that is itself a pointer type.
        template <class T>
        concept Message = std::copyable<T> && std::equality_comparable<T> && std::is_aggregate_v<T>;

        template <class... Ts>
        consteval bool all_messages(std::variant<Ts...>* /*unused*/)
        {
            return (Message<Ts> && ...);
        }

        static_assert(all_messages(static_cast<CoreToUi*>(nullptr)));
        static_assert(all_messages(static_cast<UiToCore*>(nullptr)));

    } // namespace
} // namespace stackide
