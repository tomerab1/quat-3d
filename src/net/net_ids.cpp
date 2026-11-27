#include "engine/net/net_ids.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"

namespace engine::net {

namespace {

constexpr std::uint32_t k_fnv_offset = 2166136261U;
constexpr std::uint32_t k_fnv_prime = 16777619U;
constexpr int k_max_path_depth = 256; // hierarchy cycles are rejected upstream; belt and braces

[[nodiscard]] std::uint32_t fnv1a(std::uint32_t h, std::string_view bytes) {
    for (const char c : bytes) h = (h ^ static_cast<unsigned char>(c)) * k_fnv_prime;
    return h;
}

[[nodiscard]] bool entity_index_less(entt::entity a, entt::entity b) {
    return entt::to_entity(a) < entt::to_entity(b);
}

} // namespace

std::uint32_t net_id_hash(std::string_view key) {
    const std::uint32_t h = fnv1a(k_fnv_offset, key);
    return h == 0 ? 1U : h;
}

std::string net_path(const entt::registry& registry, entt::entity e) {
    std::vector<std::string_view> parts;
    entt::entity at = e;
    for (int depth = 0; depth < k_max_path_depth && at != entt::null && registry.valid(at);
         ++depth) {
        const auto* name = registry.try_get<scene::Name>(at);
        parts.emplace_back(name != nullptr ? std::string_view(name->value)
                                           : std::string_view("?"));
        const auto* parent = registry.try_get<scene::Parent>(at);
        at = parent != nullptr ? parent->entity : entt::entity{entt::null};
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += *it;
    }
    return path;
}

NetIdReport assign_net_ids(entt::registry& registry) {
    NetIdReport report;

    // Entity-index (creation) order: identical on both ends of a shared scene.
    std::vector<entt::entity> entities(registry.view<scene::Transform>().begin(),
                                       registry.view<scene::Transform>().end());
    std::sort(entities.begin(), entities.end(), entity_index_less);

    // 1. Reserve pre-existing (authored) ids; a duplicate loses its id and is
    //    re-keyed below like any untagged entity.
    std::unordered_set<std::uint32_t> used;
    std::vector<entt::entity> candidates;
    for (const entt::entity e : entities) {
        if (auto* rep = registry.try_get<scene::NetReplicated>(e); rep != nullptr) {
            if (rep->net_id != 0) {
                if (used.insert(rep->net_id).second) {
                    ++report.kept;
                    continue;
                }
                std::fprintf(stderr, "[net] duplicate net_id %u on '%s' — reassigning\n",
                             rep->net_id, net_path(registry, e).c_str());
                ++report.duplicate_ids;
                rep->net_id = 0;
            }
            candidates.push_back(e); // explicitly replicated, id still to assign
            continue;
        }
        if (!registry.all_of<scene::Name>(e)) continue;
        // The predicted player character is owned by its client (14.3), not by
        // the replication stream.
        if (registry.all_of<scene::CharacterController>(e)) continue;
        candidates.push_back(e);
    }

    // 2. Key = path + occurrence index; hash, re-probing on collision.
    std::unordered_map<std::string, std::uint32_t> occurrences;
    for (const entt::entity e : candidates) {
        std::string key = net_path(registry, e);
        const std::uint32_t occurrence = occurrences[key]++;
        if (occurrence == 1) {
            std::fprintf(stderr, "[net] duplicate entity name '%s' — disambiguated by "
                                 "occurrence index\n", key.c_str());
        }
        if (occurrence > 0) ++report.duplicate_names;
        key += '#';
        key += std::to_string(occurrence);

        std::uint32_t id = net_id_hash(key);
        for (std::uint32_t probe = 1; used.contains(id); ++probe) {
            ++report.hash_collisions;
            id = net_id_hash(key + '~' + std::to_string(probe));
        }
        used.insert(id);
        registry.emplace_or_replace<scene::NetReplicated>(e, id);
        ++report.tagged;
    }

    std::fprintf(stderr,
                 "[net] %u entities tagged for replication (%u kept, %u duplicate names, "
                 "%u duplicate ids, %u hash collisions)\n",
                 report.tagged, report.kept, report.duplicate_names, report.duplicate_ids,
                 report.hash_collisions);
    return report;
}

} // namespace engine::net
