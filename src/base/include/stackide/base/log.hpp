//
// Created by bhaskell on 9/18/26.
//
#pragma once
#include <cstdio>
#include <print>

namespace stackide
{
    template <typename... Args>
    void log(std::format_string<Args...> fmt, Args&&... args)
    {
        std::println(stderr, fmt, std::forward<Args>(args)...);
    }
} // namespace stackide
