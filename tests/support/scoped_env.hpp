#pragma once

// Sets (or, with nullptr, removes) an environment variable for one scope and
// restores the previous value afterwards.

#include <cstdlib>
#include <string>

namespace quat::test {

inline void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        ::setenv(name, value, 1);
    } else {
        ::unsetenv(name);
    }
#endif
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
            had_ = true;
        }
        set_env(name, value);
    }
    ~ScopedEnv() { set_env(name_, had_ ? old_.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::string old_;
    bool        had_ = false;
};

} // namespace quat::test
