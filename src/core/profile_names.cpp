#include "engine/core/profile_names.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace engine::core {

namespace {

struct StringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

// Entries are written under the mutex and published by `count` (release), so
// readers on any thread see complete entries below the count they acquire.
struct NameTable {
    struct Entry {
        std::string     name;
        ProfileCategory category = ProfileCategory::scope;
    };

    NameTable() {
        entries[k_profile_overflow_name].name = "(other)";
        count.store(1, std::memory_order_release);
    }

    std::mutex                                                  mutex;
    std::array<Entry, k_max_profile_names>                     entries;
    std::unordered_map<std::string, ProfileNameId, StringHash, std::equal_to<>> ids;
    std::atomic<std::uint32_t>                                  count{0};
};

NameTable& names() {
    static NameTable table;
    return table;
}

struct ThreadTable {
    std::mutex               mutex;
    std::vector<std::string> names;
};

ThreadTable& threads() {
    static ThreadTable table;
    return table;
}

thread_local std::uint8_t t_thread_index = k_unnamed_profile_thread;

} // namespace

std::string_view to_string(ProfileCategory category) {
    switch (category) {
    case ProfileCategory::scope: return "scope";
    case ProfileCategory::system: return "system";
    case ProfileCategory::pass: return "pass";
    case ProfileCategory::wait: return "wait";
    }
    return "scope";
}

ProfileNameId intern_profile_name(std::string_view name, ProfileCategory category) {
    if (name.size() > k_max_profile_name_length) name = name.substr(0, k_max_profile_name_length);
    NameTable& t = names();
    const std::scoped_lock lock(t.mutex);
    if (const auto it = t.ids.find(name); it != t.ids.end()) return it->second;
    const std::uint32_t n = t.count.load(std::memory_order_relaxed);
    if (n >= k_max_profile_names) return k_profile_overflow_name;
    t.entries[n].name.assign(name);
    t.entries[n].category = category;
    const auto id = static_cast<ProfileNameId>(n);
    t.ids.emplace(std::string(name), id);
    t.count.store(n + 1, std::memory_order_release);
    return id;
}

ProfileNameId ProfileNameCache::id(std::string_view name) {
    if (const auto it = ids_.find(name); it != ids_.end()) return it->second;
    ++misses_;
    const ProfileNameId id = intern_profile_name(name, category_);
    // Bounded like the table: once it is full every new name is "(other)",
    // and caching those would only grow the map.
    if (ids_.size() < k_max_profile_names) ids_.emplace(std::string(name), id);
    return id;
}

std::string_view profile_name(ProfileNameId id) {
    const NameTable& t = names();
    if (id >= t.count.load(std::memory_order_acquire)) return t.entries[k_profile_overflow_name].name;
    return t.entries[id].name;
}

ProfileCategory profile_category(ProfileNameId id) {
    const NameTable& t = names();
    if (id >= t.count.load(std::memory_order_acquire)) return ProfileCategory::scope;
    return t.entries[id].category;
}

std::uint32_t profile_name_count() {
    return names().count.load(std::memory_order_acquire);
}

std::uint8_t set_profile_thread_name(std::string_view name) {
    ThreadTable& t = threads();
    const std::scoped_lock lock(t.mutex);
    if (t_thread_index != k_unnamed_profile_thread) {
        t.names[t_thread_index].assign(name);
        return t_thread_index;
    }
    if (t.names.size() >= k_unnamed_profile_thread) return k_unnamed_profile_thread;
    t.names.emplace_back(name);
    t_thread_index = static_cast<std::uint8_t>(t.names.size() - 1);
    return t_thread_index;
}

std::uint8_t profile_thread_index() {
    return t_thread_index;
}

std::vector<std::string> profile_thread_names() {
    ThreadTable& t = threads();
    const std::scoped_lock lock(t.mutex);
    return t.names;
}

} // namespace engine::core
