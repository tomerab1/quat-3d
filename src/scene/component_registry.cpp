// ComponentRegistry — see include/engine/scene/component_registry.hpp.

#include "engine/scene/component_registry.hpp"

#include <algorithm>
#include <utility>

#include "engine/core/log.hpp"

namespace engine::scene {

namespace {

[[nodiscard]] bool valid_component_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z') return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

} // namespace

ComponentRegistry::ComponentRegistry() = default;
ComponentRegistry::~ComponentRegistry() = default;

std::expected<void, core::Error> ComponentRegistry::add(ComponentCodec codec) {
    const auto fail = [&](std::string why) {
        return std::unexpected(core::Error{"component '" + codec.name + "': " + std::move(why)});
    };
    if (!valid_component_name(codec.name)) {
        return fail("a component name is 1-64 lowercase letters, digits and '_', starting "
                    "with a letter");
    }
    if (find(codec.name) != nullptr) return fail("already registered");
    if (!codec.has || !codec.encode) return fail("a codec needs `has` and `encode`");
    if (codec.writable && !codec.apply) return fail("a writable codec needs `apply`");
    if (!codec.writable && codec.addable) return fail("a read-only component cannot be addable");
    // Stable: equal orders keep registration order.
    const auto at = std::ranges::upper_bound(codecs_, codec.order, {},
                                             [](const auto& c) { return c->order; });
    codecs_.insert(at, std::make_unique<ComponentCodec>(std::move(codec)));
    rebuild_infos();
    return {};
}

bool ComponentRegistry::remove(std::string_view name) {
    const auto it = std::ranges::find_if(codecs_, [&](const auto& c) { return c->name == name; });
    if (it == codecs_.end()) return false;
    codecs_.erase(it);
    rebuild_infos();
    return true;
}

const ComponentCodec* ComponentRegistry::find(std::string_view name) const {
    for (const auto& c : codecs_) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

const ComponentCodec& ComponentRegistry::at(std::size_t index) const { return *codecs_[index]; }

void ComponentRegistry::rebuild_infos() {
    infos_.clear();
    infos_.reserve(codecs_.size());
    for (const auto& c : codecs_) infos_.push_back({c->name, c->summary, c->addable, c->writable});
}

ComponentRegistry& component_registry() {
    static ComponentRegistry registry;
    static const bool registered = [] {
        if (auto ok = register_engine_components(registry); !ok) {
            // A broken engine codec is a build defect (the EngineComponents
            // tests catch it): report it, keep what did register.
            core::log(core::LogLevel::error, "scene", "component registry: {}", ok.error().message);
        }
        return true;
    }();
    (void)registered;
    return registry;
}

} // namespace engine::scene
