//! Confirmation for destructive tools (policy `confirm`): the user is asked
//! before the call runs.
//!
//! - Protocol 2026-07-28: the call answers `input_required` with a form
//!   elicitation (MRTR, SEP-2322) and an opaque `requestState`; the client
//!   asks the user and retries with the answer. The state is a server-side
//!   handle bound to the tool and its arguments, used once.
//! - Older protocols: the server sends `elicitation/create` and waits.
//! - Clients without elicitation: the call is refused until it is repeated
//!   with `confirm: true`, which the model passes only after asking the user
//!   itself (the refusal says so).
//!
//! The question shown to the user is `content/confirm/<tool>.md`, a
//! `MiniJinja` template over the call's arguments.

use std::collections::HashMap;
use std::collections::hash_map::RandomState;
use std::hash::BuildHasher;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use rmcp::RoleServer;
use rmcp::model::{
    CallToolRequestParams, CallToolResponse, CallToolResult, ContentBlock, ElicitRequest,
    ElicitRequestParams, ElicitationAction, ElicitationSchema, InputRequest, InputRequests,
    InputRequiredResult, JsonObject, ProtocolVersion,
};
use rmcp::service::RequestContext;
use serde_json::Value;

use super::policy::{CONFIRM_ARG, Policy};
use crate::content;

/// Key of the elicitation in `inputRequests` / `inputResponses`.
pub const INPUT_KEY: &str = "confirm";
/// How long a pending confirmation stays valid.
const PENDING_TTL: Duration = Duration::from_secs(600);
/// Pending confirmations kept at once (oldest dropped first).
const MAX_PENDING: usize = 64;

/// What to do with a call that needs confirmation.
#[derive(Debug)]
pub enum Gate {
    /// Run the tool (its `confirm` argument removed).
    Proceed(CallToolRequestParams),
    /// Answer with this instead of running the tool.
    Respond(CallToolResponse),
}

#[derive(Debug)]
struct Pending {
    tool: String,
    arguments: String,
    issued: Instant,
}

/// Pending MRTR confirmations, keyed by the opaque `requestState`.
#[derive(Debug, Default)]
pub struct Confirmations {
    pending: Mutex<HashMap<String, Pending>>,
    counter: AtomicU64,
    seed: RandomState,
}

impl Confirmations {
    /// Decide whether `request` for the tool of `policy` may run now.
    pub async fn gate(
        &self,
        policy: &Policy,
        mut request: CallToolRequestParams,
        context: &RequestContext<RoleServer>,
    ) -> Gate {
        let confirmed = match request
            .arguments
            .as_mut()
            .and_then(|a| a.remove(CONFIRM_ARG))
        {
            None => false,
            Some(Value::Bool(b)) => b,
            Some(other) => {
                return respond(format!(
                    "{}: `confirm` must be true or left out, got {other}.",
                    policy.name
                ));
            }
        };
        if confirmed {
            return Gate::Proceed(request);
        }
        let arguments = canonical(request.arguments.as_ref());

        // An MRTR retry: the state must be one we issued for this call.
        if let Some(state) = request.request_state.take() {
            let issued = self.take(&state);
            let matches = issued
                .as_ref()
                .is_some_and(|p| p.tool == policy.name && p.arguments == arguments);
            if !matches {
                return respond(format!(
                    "{}: the confirmation is unknown, expired or for different arguments; \
                     call the tool again to ask the user.",
                    policy.name
                ));
            }
            let answer = request
                .input_responses
                .take()
                .and_then(|mut r| r.remove(INPUT_KEY));
            return match answer.as_ref().map(accepted) {
                Some(true) => Gate::Proceed(request),
                _ => declined(policy),
            };
        }

        let question = question(policy, request.arguments.as_ref());
        if !client_can_elicit(context) {
            return respond(format!(
                "{name} needs the user's confirmation and this client cannot ask them \
                 (no elicitation support). Ask the user: \"{question}\" Call {name} again \
                 with `confirm: true` only if they agree.",
                name = policy.name
            ));
        }
        let params = ElicitRequestParams::FormElicitationParams {
            meta: None,
            message: question,
            requested_schema: confirm_schema(policy),
        };
        let modern = context
            .protocol_version()
            .is_some_and(|v| v >= ProtocolVersion::V_2026_07_28);
        if modern {
            let state = self.issue(policy.name, arguments);
            let mut requests = InputRequests::new();
            requests.insert(
                INPUT_KEY.to_owned(),
                InputRequest::Elicitation(ElicitRequest::new(params)),
            );
            return Gate::Respond(InputRequiredResult::new(Some(requests), Some(state)).into());
        }
        match context.peer.create_elicitation(params).await {
            Ok(result) if result.action == ElicitationAction::Accept => {
                let answer = result.content.unwrap_or(Value::Null);
                if answer.get(INPUT_KEY) == Some(&Value::Bool(true)) {
                    Gate::Proceed(request)
                } else {
                    declined(policy)
                }
            }
            Ok(_) => declined(policy),
            Err(e) => respond(format!(
                "{name}: could not ask the user for confirmation ({e}). Ask them yourself and \
                 call {name} again with `confirm: true` only if they agree.",
                name = policy.name
            )),
        }
    }

    fn issue(&self, tool: &str, arguments: String) -> String {
        let n = self.counter.fetch_add(1, Ordering::Relaxed);
        let state = format!(
            "confirm-{:016x}{n:x}",
            self.seed.hash_one((n, Instant::now()))
        );
        let mut pending = self
            .pending
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        pending.retain(|_, p| p.issued.elapsed() < PENDING_TTL);
        if pending.len() >= MAX_PENDING
            && let Some(oldest) = pending
                .iter()
                .min_by_key(|(_, p)| p.issued)
                .map(|(k, _)| k.clone())
        {
            pending.remove(&oldest);
        }
        pending.insert(
            state.clone(),
            Pending {
                tool: tool.to_owned(),
                arguments,
                issued: Instant::now(),
            },
        );
        state
    }

    fn take(&self, state: &str) -> Option<Pending> {
        let mut pending = self
            .pending
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        pending
            .remove(state)
            .filter(|p| p.issued.elapsed() < PENDING_TTL)
    }
}

fn respond(text: String) -> Gate {
    Gate::Respond(CallToolResult::error(vec![ContentBlock::text(text)]).into())
}

fn declined(policy: &Policy) -> Gate {
    respond(format!(
        "{}: the user did not confirm, so nothing was changed. Do not retry unless they ask.",
        policy.name
    ))
}

/// Whether an `ElicitResult` (as JSON) accepted with `confirm: true`.
fn accepted(result: &Value) -> bool {
    result.get("action").and_then(Value::as_str) == Some("accept")
        && result.pointer("/content/confirm") == Some(&Value::Bool(true))
}

fn client_can_elicit(context: &RequestContext<RoleServer>) -> bool {
    context.client_capabilities().is_some_and(|c| {
        c.elicitation
            .as_ref()
            .is_some_and(|e| e.form.is_some() || e.url.is_none())
    })
}

/// Arguments in a stable textual form, to bind a confirmation to them.
fn canonical(arguments: Option<&JsonObject>) -> String {
    // serde_json's map is ordered (no `preserve_order`), so this is canonical.
    arguments.map_or_else(String::new, |a| Value::Object(a.clone()).to_string())
}

fn confirm_schema(policy: &Policy) -> ElicitationSchema {
    ElicitationSchema::builder()
        .required_bool_property(INPUT_KEY, |b| b.title(policy.title.to_owned()))
        .build_unchecked()
}

/// The question for the user, from `content/confirm/<tool>.md`.
#[must_use]
pub fn question(policy: &Policy, arguments: Option<&JsonObject>) -> String {
    let args = arguments.cloned().unwrap_or_default();
    content::render_document(&format!("confirm/{}", policy.name), &args).map_or_else(
        |e| {
            tracing::error!(
                "confirmation template for {} does not render: {e}",
                policy.name
            );
            format!("Confirm: {}?", policy.title)
        },
        |s| s.trim().to_owned(),
    )
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;
    use crate::server::policy::{POLICIES, get};

    #[test]
    fn every_confirming_tool_has_a_question() {
        let args = json!({"path": "levels/a.json", "name": "lava"});
        for p in POLICIES.iter().filter(|p| p.confirm) {
            let q = question(p, args.as_object());
            assert!(q.contains('?') && q.len() > 30, "{}: {q}", p.name);
            assert!(!q.starts_with("Confirm:"), "{}: template missing", p.name);
        }
        // A missing argument falls back to a generic question, not an error.
        let q = question(get("scene_load").unwrap(), None);
        assert_eq!(q, "Confirm: Load scene file?");
    }

    #[test]
    fn questions_name_their_target() {
        let args = json!({"path": "scripts/coin.lua"});
        let q = question(get("script_delete").unwrap(), args.as_object());
        assert!(q.contains("scripts/coin.lua"), "{q}");
        let q = question(
            get("scene_clear").unwrap(),
            json!({"keep_lights": false}).as_object(),
        );
        assert!(q.contains("lights"), "{q}");
    }

    #[test]
    fn states_are_single_use_and_bound() {
        let c = Confirmations::default();
        let s = c.issue("scene_clear", "{}".into());
        let s2 = c.issue("scene_clear", "{}".into());
        assert_ne!(s, s2);
        assert_eq!(c.take(&s).map(|p| p.tool), Some("scene_clear".into()));
        assert!(c.take(&s).is_none(), "single use");
        for i in 0..MAX_PENDING + 5 {
            c.issue("scene_load", format!("{i}"));
        }
        assert!(c.pending.lock().unwrap().len() <= MAX_PENDING);
    }

    #[test]
    fn only_accept_with_true_confirms() {
        assert!(accepted(
            &json!({"action":"accept","content":{"confirm":true}})
        ));
        assert!(!accepted(
            &json!({"action":"accept","content":{"confirm":false}})
        ));
        assert!(!accepted(&json!({"action":"decline"})));
        assert!(!accepted(&json!({"action":"accept"})));
    }
}
