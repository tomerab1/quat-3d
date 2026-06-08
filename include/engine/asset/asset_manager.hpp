#pragma once

// Asset manager skeleton (Phase 3, Slice 3.1).
//
// Provides stable content identity (AssetId = xxHash64 of a relative path),
// ref-counted nullable handles (AssetHandle<T>), and a synchronous load path
// with per-type default fallback assets. Async loading and hot-reload are
// deferred to Phase 6 — for now `load()` runs the supplied loader inline.
//
// The manager is type-agnostic: asset types (MeshAsset, TextureAsset, ...) are
// supplied as template parameters and need no registration beyond an optional
// default value. Per-type caches are keyed without RTTI (the engine builds with
// -fno-rtti) via a stable address-per-type tag.

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "engine/core/error.hpp"

namespace engine::asset {

// Stable content identity derived from an asset's relative path. Equality and
// hashing operate on the 64-bit value so AssetId can key unordered containers.
struct AssetId {
    std::uint64_t value = 0;

    [[nodiscard]] bool operator==(const AssetId&) const noexcept = default;
};

// xxHash64 (seed 0) of the lexically-normalised, forward-slash form of the
// path so the same logical asset hashes identically across platforms.
[[nodiscard]] AssetId asset_id_from_path(std::string_view relative_path);

// Lifecycle of a slot. `loading` is reserved for the Phase 6 async path; the
// synchronous loader transitions not_loaded -> loaded or not_loaded -> failed.
enum class AssetState : std::uint8_t { not_loaded, loading, loaded, failed };

// Shared storage backing every handle to a given asset. The manager owns one
// slot per (type, id); handles hold a shared_ptr to it, so the asset lives as
// long as either the cache or any outstanding handle references it. `value` is
// meaningful only once `state == loaded`; until then (or on failure) a handle
// resolves to `fallback`, the per-type default shared by every unloaded slot.
// Sharing the default by pointer — rather than copying it into each slot — also
// lets move-only assets (e.g. a MeshAsset owning GpuBuffers) be cached.
template <typename T>
struct AssetSlot {
    AssetId id{};
    std::atomic<AssetState> state{AssetState::not_loaded};
    T value{};
    std::shared_ptr<const T> fallback;
};

// Ref-counted, nullable handle to an asset of type T. Copyable and cheap.
// Dereferencing a handle returned by the manager yields the loaded asset, or
// the per-type default while the asset is not yet loaded / failed to load.
// A default-constructed (null) handle must not be dereferenced — check valid().
template <typename T>
class AssetHandle {
public:
    AssetHandle() = default;
    explicit AssetHandle(std::shared_ptr<AssetSlot<T>> slot) noexcept
        : slot_(std::move(slot)) {}

    [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] AssetId id() const noexcept { return slot_ ? slot_->id : AssetId{}; }
    [[nodiscard]] AssetState state() const noexcept {
        return slot_ ? slot_->state.load(std::memory_order_acquire) : AssetState::not_loaded;
    }
    [[nodiscard]] bool is_loaded() const noexcept { return state() == AssetState::loaded; }

    // Precondition: valid(). Returns the loaded asset, else the per-type default.
    [[nodiscard]] const T& operator*() const noexcept { return resolve(); }
    [[nodiscard]] const T* operator->() const noexcept { return &resolve(); }

private:
    [[nodiscard]] const T& resolve() const noexcept {
        return slot_->state.load(std::memory_order_acquire) == AssetState::loaded
                   ? slot_->value
                   : *slot_->fallback;
    }

    std::shared_ptr<AssetSlot<T>> slot_;
};

// Per-type cache: maps AssetId -> slot, plus the shared default asset that
// unloaded slots resolve to. Type-erased behind a void shared_ptr in the
// manager (see AssetManager::cache_for).
template <typename T>
class AssetCache {
public:
    void set_default(T value) { default_ = std::make_shared<const T>(std::move(value)); }

    // The shared default, lazily materialised to a default-constructed T so a
    // handle is always dereferenceable even if no explicit default was set.
    [[nodiscard]] std::shared_ptr<const T> default_ptr() {
        if (!default_) default_ = std::make_shared<const T>();
        return default_;
    }

    [[nodiscard]] std::shared_ptr<AssetSlot<T>> find(AssetId id) const {
        const auto it = slots_.find(id.value);
        return it == slots_.end() ? nullptr : it->second;
    }
    void insert(std::shared_ptr<AssetSlot<T>> slot) { slots_[slot->id.value] = std::move(slot); }

    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

private:
    std::unordered_map<std::uint64_t, std::shared_ptr<AssetSlot<T>>> slots_;
    std::shared_ptr<const T> default_;
};

class AssetManager {
public:
    AssetManager() = default;

    // Root that relative paths resolve against when handed to a loader. The
    // AssetId is computed from the relative path, independent of this root.
    void set_asset_root(std::filesystem::path root) { asset_root_ = std::move(root); }
    [[nodiscard]] const std::filesystem::path& asset_root() const noexcept { return asset_root_; }

    // Register the fallback asset returned while a T is unloaded or failed.
    template <typename T>
    void set_default(T value) {
        cache_for<T>().set_default(std::move(value));
    }

    // Synchronously load (or fetch the cached) asset at `relative_path`.
    //
    // Loader is any callable `(const std::filesystem::path& absolute)
    // -> std::expected<T, core::Error>`. On a cache hit the existing handle is
    // returned and the loader is not invoked. On a miss the slot is created
    // pre-filled with the default, the loader runs inline, and the slot
    // transitions to loaded (value replaced) or failed (default retained).
    template <typename T, typename Loader>
    AssetHandle<T> load(std::string_view relative_path, Loader&& loader) {
        AssetCache<T>& cache = cache_for<T>();
        const AssetId id = asset_id_from_path(relative_path);
        if (auto existing = cache.find(id)) return AssetHandle<T>(std::move(existing));

        auto slot = std::make_shared<AssetSlot<T>>();
        slot->id = id;
        slot->fallback = cache.default_ptr();
        cache.insert(slot);

        const std::filesystem::path absolute = asset_root_ / std::filesystem::path(relative_path);
        std::expected<T, core::Error> result = std::forward<Loader>(loader)(absolute);
        if (result) {
            slot->value = std::move(*result);
            slot->state.store(AssetState::loaded, std::memory_order_release);
        } else {
            slot->state.store(AssetState::failed, std::memory_order_release);
        }
        return AssetHandle<T>(std::move(slot));
    }

    // Fetch a handle to an already-known asset, or a null handle if unknown.
    template <typename T>
    [[nodiscard]] AssetHandle<T> get(AssetId id) {
        if (auto slot = cache_for<T>().find(id)) return AssetHandle<T>(std::move(slot));
        return {};
    }

    template <typename T>
    [[nodiscard]] std::size_t count() {
        return cache_for<T>().size();
    }

private:
    // Stable, RTTI-free per-type tag: the address of a unique static is distinct
    // for each instantiation and constant for the program's lifetime.
    template <typename T>
    static const void* type_tag() noexcept {
        static const char tag = 0;
        return &tag;
    }

    template <typename T>
    AssetCache<T>& cache_for() {
        const void* key = type_tag<T>();
        auto it = caches_.find(key);
        if (it == caches_.end()) {
            it = caches_.emplace(key, std::make_shared<AssetCache<T>>()).first;
        }
        return *std::static_pointer_cast<AssetCache<T>>(it->second);
    }

    std::filesystem::path asset_root_;
    std::unordered_map<const void*, std::shared_ptr<void>> caches_;
};

// Exercises id stability, cache identity, default fallback, and load/fail
// transitions with an in-memory dummy asset. Returns an error on mismatch.
[[nodiscard]] std::expected<void, core::Error> run_asset_manager_self_test();

} // namespace engine::asset
