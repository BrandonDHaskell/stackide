#pragma once

#include <utility>

namespace stackide {

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F fn) noexcept : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { if (armed_) fn_(); }
    void release() noexcept { armed_ = false; }

private:
    F fn_;
    bool armed_ = true;
};

} // namespace stackide
