//! Resources: the fixed ones (scene summary, scripting reference, skill
//! files), the templates that address engine objects (`quat://entity/…`,
//! `quat://script/…`, `quat://shader/…`), completions for the template
//! arguments, and the resource links tools return for what they create.

use percent_encoding::{AsciiSet, NON_ALPHANUMERIC, percent_decode_str, utf8_percent_encode};
use rmcp::ErrorData;
use rmcp::model::{
    CompleteRequestParams, CompleteResult, CompletionInfo, ContentBlock, ReadResourceResult,
    Reference, Resource, ResourceContents, ResourceTemplate,
};
use serde_json::{Value, json};

use super::{QuatServer, SCENE_SUMMARY_URI, SCRIPTING_REFERENCE_URI};
use crate::client::EngineError;
use crate::content;
use crate::skills;

/// Template of one entity, read as `scene.get` JSON.
pub const ENTITY_TEMPLATE: &str = "quat://entity/{entity}";
/// Template of one Lua script's source.
pub const SCRIPT_TEMPLATE: &str = "quat://script/{+path}";
/// Template of one material shader's source.
pub const SHADER_TEMPLATE: &str = "quat://shader/{name}";

const ENTITY_PREFIX: &str = "quat://entity/";
const SCRIPT_PREFIX: &str = "quat://script/";
const SHADER_PREFIX: &str = "quat://shader/";

/// Characters escaped in a URI path segment (script paths keep their `/`).
const SEGMENT: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'_')
    .remove(b'.')
    .remove(b'~');
const PATH: &AsciiSet = &SEGMENT.remove(b'/');

/// The fixed resources (skill files are added by the server when served).
#[must_use]
pub fn fixed() -> Vec<Resource> {
    vec![
        Resource::new(SCENE_SUMMARY_URI, "scene_summary")
            .with_title("Current scene summary")
            .with_description(include_str!("../../content/resources/scene_summary.md").trim())
            .with_mime_type("application/json"),
        Resource::new(SCRIPTING_REFERENCE_URI, "scripting_reference")
            .with_title("Lua scripting reference")
            .with_description(include_str!("../../content/resources/scripting_reference.md").trim())
            .with_mime_type("text/markdown"),
    ]
}

/// The resource templates.
#[must_use]
pub fn templates() -> Vec<ResourceTemplate> {
    vec![
        ResourceTemplate::new(ENTITY_TEMPLATE, "entity")
            .with_title("Entity")
            .with_description(include_str!("../../content/resources/entity.md").trim())
            .with_mime_type("application/json"),
        ResourceTemplate::new(SCRIPT_TEMPLATE, "script")
            .with_title("Lua script")
            .with_description(include_str!("../../content/resources/script.md").trim())
            .with_mime_type("text/x-lua"),
        ResourceTemplate::new(SHADER_TEMPLATE, "shader")
            .with_title("Material shader")
            .with_description(include_str!("../../content/resources/shader.md").trim())
            .with_mime_type("text/x-slang"),
    ]
}

/// `quat://entity/<id>`.
#[must_use]
pub fn entity_uri(id: u64) -> String {
    format!("{ENTITY_PREFIX}{id}")
}

/// `quat://script/<path>`.
#[must_use]
pub fn script_uri(path: &str) -> String {
    format!("{SCRIPT_PREFIX}{}", utf8_percent_encode(path, PATH))
}

/// `quat://shader/<name>`.
#[must_use]
pub fn shader_uri(name: &str) -> String {
    format!("{SHADER_PREFIX}{}", utf8_percent_encode(name, SEGMENT))
}

/// Resource links for what tool `name` created or changed, from its reply.
#[must_use]
pub fn links(name: &str, reply: &Value) -> Vec<ContentBlock> {
    let link = match name {
        "scene_create" | "asset_spawn_model" => reply
            .get("id")
            .and_then(Value::as_u64)
            .map(|id| entity_link(id, reply.get("name").and_then(Value::as_str))),
        "script_create" | "script_update" => reply
            .get("path")
            .and_then(Value::as_str)
            .map(|p| Resource::new(script_uri(p), p).with_mime_type("text/x-lua")),
        "shader_create" | "shader_update" => reply
            .get("name")
            .and_then(Value::as_str)
            .map(|n| Resource::new(shader_uri(n), n).with_mime_type("text/x-slang")),
        _ => None,
    };
    link.map(ContentBlock::resource_link).into_iter().collect()
}

fn entity_link(id: u64, name: Option<&str>) -> Resource {
    Resource::new(entity_uri(id), name.unwrap_or("entity")).with_mime_type("application/json")
}

fn decode(segment: &str) -> Result<String, ErrorData> {
    percent_decode_str(segment)
        .decode_utf8()
        .map(std::borrow::Cow::into_owned)
        .map_err(|_| ErrorData::invalid_params("resource URI is not valid UTF-8", None))
}

/// An entity reference from a URI: a numeric id or a unique name.
fn entity_ref(segment: &str) -> Value {
    segment
        .parse::<u64>()
        .map_or_else(|_| json!(segment), |id| json!(id))
}

fn engine_error(uri: &str, e: &EngineError) -> ErrorData {
    match e.rpc_code() {
        Some(crate::client::code::NOT_FOUND) => {
            ErrorData::resource_not_found(format!("{uri}: {e}"), None)
        }
        Some(crate::client::code::INVALID_PARAMS) => {
            ErrorData::invalid_params(format!("{uri}: {e}"), None)
        }
        _ => ErrorData::internal_error(format!("{uri}: {e}"), None),
    }
}

impl QuatServer {
    /// Read `uri`: a fixed resource, a skill file or a template instance.
    pub(super) async fn read_uri(&self, uri: &str) -> Result<ReadResourceResult, ErrorData> {
        if uri == SCRIPTING_REFERENCE_URI {
            return Ok(ReadResourceResult::new(vec![
                ResourceContents::text(content::SCRIPTING_REFERENCE, uri)
                    .with_mime_type("text/markdown"),
            ]));
        }
        if uri == SCENE_SUMMARY_URI {
            let summary = self
                .scene_summary()
                .await
                .map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
            let text =
                serde_json::to_string_pretty(&summary).unwrap_or_else(|_| summary.to_string());
            return Ok(ReadResourceResult::new(vec![
                ResourceContents::text(text, uri).with_mime_type("application/json"),
            ]));
        }
        if let Some(entity) = uri.strip_prefix(ENTITY_PREFIX).filter(|s| !s.is_empty()) {
            let entity = entity_ref(&decode(entity)?);
            let reply = self
                .engine
                .call("scene.get", Some(json!({ "entity": entity })))
                .await
                .map_err(|e| engine_error(uri, &e))?;
            return Ok(ReadResourceResult::new(vec![
                ResourceContents::text(reply.to_string(), uri).with_mime_type("application/json"),
            ]));
        }
        if let Some(path) = uri.strip_prefix(SCRIPT_PREFIX).filter(|s| !s.is_empty()) {
            let params = json!({ "path": decode(path)? });
            return self
                .read_source(uri, "script.get", params, "text/x-lua")
                .await;
        }
        if let Some(name) = uri.strip_prefix(SHADER_PREFIX).filter(|s| !s.is_empty()) {
            let params = json!({ "name": decode(name)? });
            return self
                .read_source(uri, "shader.get", params, "text/x-slang")
                .await;
        }
        let skills = if self.workflows.skills {
            format!(", {}", skills::catalog().uris())
        } else {
            String::new()
        };
        Err(ErrorData::resource_not_found(
            format!(
                "unknown resource `{uri}`; available: {SCENE_SUMMARY_URI}, \
                 {SCRIPTING_REFERENCE_URI}{skills}, and the templates {ENTITY_TEMPLATE}, \
                 {SCRIPT_TEMPLATE}, {SHADER_TEMPLATE}"
            ),
            None,
        ))
    }

    async fn read_source(
        &self,
        uri: &str,
        method: &str,
        params: Value,
        mime: &str,
    ) -> Result<ReadResourceResult, ErrorData> {
        let reply = self
            .engine
            .call(method, Some(params))
            .await
            .map_err(|e| engine_error(uri, &e))?;
        let source = reply.get("source").and_then(Value::as_str).ok_or_else(|| {
            ErrorData::internal_error(format!("{uri}: {method} returned no `source`"), None)
        })?;
        Ok(ReadResourceResult::new(vec![
            ResourceContents::text(source, uri).with_mime_type(mime),
        ]))
    }

    /// `completion/complete` for the template arguments. Prompt arguments
    /// are free text and get no suggestions. Completion is best effort: when
    /// the engine is not reachable the list is empty.
    pub(super) async fn complete_argument(
        &self,
        request: &CompleteRequestParams,
    ) -> Result<CompleteResult, ErrorData> {
        let prefix = request.argument.value.as_str();
        let values = match (&request.r#ref, request.argument.name.as_str()) {
            (Reference::Resource(r), "entity") if r.uri == ENTITY_TEMPLATE => {
                self.names(
                    "scene.list",
                    json!({"name_contains": prefix, "limit": 200}),
                    "entities",
                    "name",
                )
                .await
            }
            (Reference::Resource(r), "path") if r.uri == SCRIPT_TEMPLATE => {
                self.names("script.list", json!({}), "scripts", "path")
                    .await
            }
            (Reference::Resource(r), "name") if r.uri == SHADER_TEMPLATE => {
                self.names("shader.list", json!({}), "shaders", "name")
                    .await
            }
            (Reference::Resource(r), arg) => {
                return Err(ErrorData::invalid_params(
                    format!(
                        "no completion for argument `{arg}` of `{}`; templates: \
                         {ENTITY_TEMPLATE} (entity), {SCRIPT_TEMPLATE} (path), \
                         {SHADER_TEMPLATE} (name)",
                        r.uri
                    ),
                    None,
                ));
            }
            // Prompt arguments are free text.
            _ => Vec::new(),
        };
        let mut values: Vec<String> = values.into_iter().filter(|v| v.contains(prefix)).collect();
        values.sort();
        values.dedup();
        let total = values.len();
        values.truncate(CompletionInfo::MAX_VALUES);
        let has_more = total > values.len();
        let completion =
            CompletionInfo::with_pagination(values, u32::try_from(total).ok(), has_more)
                .map_err(|e| ErrorData::internal_error(e, None))?;
        Ok(CompleteResult::new(completion))
    }

    /// The string field `field` of every element of `list` in `method`'s reply.
    async fn names(&self, method: &str, params: Value, list: &str, field: &str) -> Vec<String> {
        let Ok(reply) = self.engine.call(method, Some(params)).await else {
            return Vec::new();
        };
        reply
            .get(list)
            .and_then(Value::as_array)
            .map(|items| {
                items
                    .iter()
                    .filter_map(|i| i.get(field).and_then(Value::as_str))
                    .filter(|s| !s.is_empty())
                    .map(str::to_owned)
                    .collect()
            })
            .unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uris_round_trip() {
        assert_eq!(entity_uri(7), "quat://entity/7");
        assert_eq!(
            script_uri("ai/patrol guard.lua"),
            "quat://script/ai/patrol%20guard.lua"
        );
        assert_eq!(
            decode("ai/patrol%20guard.lua").unwrap(),
            "ai/patrol guard.lua"
        );
        assert_eq!(shader_uri("lava"), "quat://shader/lava");
        assert_eq!(entity_ref("12"), json!(12));
        assert_eq!(entity_ref("Tower"), json!("Tower"));
    }

    #[test]
    fn links_point_at_what_was_made() {
        let l = links("scene_create", &json!({"id": 3, "name": "Box"}));
        let r = l[0].as_resource_link().expect("link");
        assert_eq!(r.uri, "quat://entity/3");
        assert_eq!(r.name, "Box");
        assert!(links("scene_create", &json!({"error": 1})).is_empty());
        assert!(links("scene_list", &json!({"id": 3})).is_empty());
        let l = links("script_create", &json!({"path": "coin.lua"}));
        assert_eq!(
            l[0].as_resource_link().unwrap().uri,
            "quat://script/coin.lua"
        );
    }

    #[test]
    fn templates_have_descriptions() {
        for t in templates() {
            assert!(
                t.description.as_deref().is_some_and(|d| d.len() > 30),
                "{}",
                t.name
            );
        }
    }
}
