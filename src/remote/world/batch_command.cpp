// scene.batch — many commands in one round trip, with "@N" back-references.

#include <charconv>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_commands = 4096;

struct Command {
    std::string method;
    Json        params = Json::object();
};

[[nodiscard]] Json error_json(const RpcError& e) {
    return Json{{"code", e.code}, {"message", e.message}};
}

// "@N" -> N, or -1 when `s` is not a back-reference.
[[nodiscard]] long back_reference(const std::string& s) {
    if (s.size() < 2 || s[0] != '@') return -1;
    long n = -1;
    const auto [end, ec] = std::from_chars(s.data() + 1, s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size() || n < 0) return -1;
    return n;
}

// The entity id a finished command produced: its `id`, or the single entry
// of `roots` (asset.spawn_model).
[[nodiscard]] const Json* produced_id(const Json& result) {
    if (!result.is_object()) return nullptr;
    if (const auto it = result.find("id"); it != result.end() && it->is_number_integer()) {
        return &*it;
    }
    if (const auto it = result.find("roots");
        it != result.end() && it->is_array() && !it->empty() && (*it)[0].is_number_integer()) {
        return &(*it)[0];
    }
    return nullptr;
}

// Replaces "@N" in the top-level `entity` / `parent` fields with the id
// command N returned.
[[nodiscard]] std::expected<void, RpcError> substitute(Json& params, std::size_t self,
                                                       const std::vector<RpcResult>& done) {
    for (const char* key : {"entity", "parent"}) {
        const auto it = params.find(key);
        if (it == params.end() || !it->is_string()) continue;
        const long n = back_reference(it->get_ref<const std::string&>());
        if (n < 0) continue;
        const std::string where = std::string(key) + ": \"@" + std::to_string(n) + "\"";
        if (static_cast<std::size_t>(n) >= self) {
            return bad_params(where + " must refer to an earlier command (commands are numbered "
                                      "from 0)");
        }
        const RpcResult& prior = done[static_cast<std::size_t>(n)];
        if (!prior) return bad_params(where + " refers to a command that failed");
        const Json* id = produced_id(*prior);
        if (id == nullptr) return bad_params(where + " refers to a command that returned no id");
        *it = *id;
    }
    return {};
}

RpcResult scene_batch(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    const Json* list = f.raw("commands");
    bool stop_on_error = true;
    f.boolean("stop_on_error", stop_on_error);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (list == nullptr || !list->is_array()) {
        return bad_params("commands: expected an array of {method, params}");
    }
    if (list->size() > k_max_commands) {
        return bad_params("commands: at most " + std::to_string(k_max_commands) +
                          " commands per batch");
    }

    // Validate the whole batch's shape before running any of it.
    std::vector<Command> commands;
    commands.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i) {
        const std::string ctx_name = "commands[" + std::to_string(i) + "]";
        core::JsonFields c((*list)[i], ctx_name);
        Command cmd;
        c.string("method", cmd.method, 128);
        const Json* p = c.raw("params");
        if (auto ok = finish(c); !ok) return std::unexpected(ok.error());
        if (cmd.method.empty()) return bad_params(ctx_name + ".method: required");
        if (cmd.method == "scene.batch") return bad_params(ctx_name + ": batches cannot nest");
        if (p != nullptr && !p->is_null()) {
            if (!p->is_object()) return bad_params(ctx_name + ".params: expected an object");
            cmd.params = *p;
        }
        commands.push_back(std::move(cmd));
    }

    std::vector<RpcResult> done;
    done.reserve(commands.size());
    Json results = Json::array();
    std::size_t first_failure = commands.size();
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (first_failure < i && stop_on_error) {
            const RpcError skipped{rpc_code::failed,
                                   "not run: command " + std::to_string(first_failure) +
                                       " failed and stop_on_error is set",
                                   nullptr};
            results.push_back({{"ok", false}, {"error", error_json(skipped)}});
            done.emplace_back(std::unexpected(skipped));
            continue;
        }
        Command& cmd = commands[i];
        RpcResult r = substitute(cmd.params, i, done).and_then([&]() {
            return ctx.server->call(cmd.method, cmd.params);
        });
        if (r) {
            results.push_back({{"ok", true}, {"result", *r}});
        } else {
            results.push_back({{"ok", false}, {"error", error_json(r.error())}});
            if (first_failure == commands.size()) first_failure = i;
        }
        done.push_back(std::move(r));
    }
    return Json{{"results", std::move(results)}};
}

} // namespace

void register_batch_command(RpcServer& server, const ContextPtr& ctx) {
    server.add_method(
        "scene.batch",
        "Run several commands in order: {commands:[{method, params}], stop_on_error?=true} -> "
        "{results:[{ok, result} | {ok:false, error}]}. \"@N\" in an entity/parent field is the id "
        "returned by command N (0-based). Not atomic; deferred methods cannot be batched.",
        [ctx](const Json& p) { return scene_batch(*ctx, p); });
}

} // namespace engine::remote::world
