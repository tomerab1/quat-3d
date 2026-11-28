#pragma once

// The JSON Schema subset game-module methods declare their parameters in
// (ROADMAP G17.1). Remote parameters are untrusted: the engine validates
// them against the method's schema before the handler runs, so a handler
// reads the shape it declared.
//
// Supported keywords (draft 2020-12 semantics): type (a name or an array of
// names), properties, required, additionalProperties (boolean), items,
// minItems, maxItems, enum, const, minimum, maximum, exclusiveMinimum,
// exclusiveMaximum, minLength, maxLength (code points), anyOf, plus the
// annotations $schema, title, description, default, examples, readOnly.
// A schema using any other keyword is refused at registration, so a schema is
// never silently weaker than it reads.

#include <expected>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::game {

// Checks that `schema` uses only the supported subset; the error names the
// offending path ("properties.x.pattern: unsupported keyword").
[[nodiscard]] std::expected<void, core::Error> check_schema(const nlohmann::json& schema);

// Validates `value` against a schema check_schema accepted. The error names
// the value's path, e.g. "params.size[1]: expected a number <= 64".
[[nodiscard]] std::expected<void, core::Error> validate_json(const nlohmann::json& schema,
                                                             const nlohmann::json& value,
                                                             const std::string& path = "params");

} // namespace engine::game
