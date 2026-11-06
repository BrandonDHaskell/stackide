# cmake/Boundaries.cmake
#
# Enforces the module boundary in docs/adr/0001-ui-core-split.md at configure time.
# Call after every target exists, since a link can't be followed before then.

include_guard(GLOBAL)

# stackide_forbid_links(<target> <regex>)
#
# Fails configuration if <target> reaches a name matching <regex> through any chain
# of links. Static libraries record private links as $<LINK_ONLY:...>, so those are
# followed too. Conditional generator expressions are not evaluated and so not followed.
function(stackide_forbid_links target pattern)
    set(pending "${target}")
    set(seen "")
    set(chain_${target} "${target}")

    while(pending)
        list(POP_FRONT pending current)

        if(NOT TARGET "${current}")
            continue()
        endif()

        get_target_property(aliased "${current}" ALIASED_TARGET)
        set(link_names "")
        if(aliased)
            set(link_names "${aliased}")
        else()
            foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
                get_target_property(values "${current}" ${property})
                if(values)
                    list(APPEND link_names ${values})
                endif()
            endforeach()
        endif()

        foreach(raw_name IN LISTS link_names)
            string(REGEX REPLACE "^\\$<(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE):(.*)>$" "\\2" name "${raw_name}")
            if(name MATCHES "^\\$<" OR name IN_LIST seen)
                continue()
            endif()

            list(APPEND seen "${name}")
            set(chain_${name} "${chain_${current}} -> ${name}")

            if(name MATCHES "${pattern}")
                message(FATAL_ERROR
                        "${target} must not link ${name} (${chain_${name}}). "
                        "See docs/adr/0001-ui-core-split.md.")
            endif()

            list(APPEND pending "${name}")
        endforeach()
    endwhile()
endfunction()
