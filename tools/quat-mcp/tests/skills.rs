//! The workflows over MCP, end to end through an in-process client: the
//! skills extension (SEP-2640), the `skill://` resources and the prompts
//! generated from the same skills.

mod common;

use std::collections::BTreeSet;
use std::sync::Arc;
use std::time::Duration;

use quat_mcp::server::Workflows;
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{
    ErrorCode, GetPromptRequestParams, GetSkillRequestParams, ReadResourceRequestParams,
    ResourceContents, SKILLS_EXTENSION_ID, SkillResources,
};
use rmcp::service::{RunningService, ServiceError};
use rmcp::{RoleClient, ServiceExt};
use serde_json::json;

/// The skills every build must serve (ROADMAP A12, plus the two workflows
/// that were prompts before).
const SKILLS: &[&str] = &[
    "author-material-shader",
    "build-level",
    "debug-physics-glitch",
    "debug-session",
    "design-npc-behaviour",
    "find-performance-problem",
    "inspect-scene",
    "playtest",
    "set-weather",
    "tune-look",
    "write-gameplay-script",
];

async fn connect(workflows: Workflows) -> RunningService<RoleClient, ()> {
    // No tool is called, so no engine needs to listen.
    let mut cfg = EngineConfig::new("127.0.0.1:9");
    cfg.timeout = Duration::from_secs(1);
    let server = QuatServer::new(Arc::new(EngineClient::new(cfg)), Duration::from_secs(1))
        .with_workflows(workflows);
    let (server_io, client_io) = tokio::io::duplex(1 << 20);
    tokio::spawn(async move {
        let running = server.serve(server_io).await.expect("server starts");
        let _ = running.waiting().await;
    });
    common::client(client_io).await
}

fn mcp_code(error: &ServiceError) -> Option<ErrorCode> {
    match error {
        ServiceError::McpError(e) => Some(e.code),
        _ => None,
    }
}

#[tokio::test]
async fn skills_are_listed_and_their_files_match_the_manifests() {
    let client = connect(Workflows::default()).await;
    let info = client.peer_info().expect("server info");
    assert!(info.capabilities.supports_skills());
    assert!(info.capabilities.resources.is_some());
    let extensions = info.capabilities.extensions.as_ref().unwrap();
    assert!(extensions.contains_key(SKILLS_EXTENSION_ID));
    let instructions = info.instructions.as_deref().unwrap();
    assert!(
        instructions.contains("skill://build-level/SKILL.md"),
        "{instructions}"
    );

    let skills = client.list_all_skills().await.unwrap();
    let names: BTreeSet<&str> = skills.iter().map(|s| s.frontmatter.name.as_str()).collect();
    assert_eq!(names, SKILLS.iter().copied().collect());

    for skill in &skills {
        assert_eq!(
            skill.uri,
            format!("skill://{}/SKILL.md", skill.frontmatter.name)
        );
        assert!(skill.frontmatter.extra.contains_key("metadata"));
        let SkillResources::Files(files) = &skill.resources else {
            panic!("{}: dynamic", skill.uri);
        };
        for file in files {
            let read = client
                .read_resource(ReadResourceRequestParams::new(file.uri.clone()))
                .await
                .unwrap();
            let ResourceContents::TextResourceContents {
                text, mime_type, ..
            } = &read.contents[0]
            else {
                panic!("{}: text expected", file.uri);
            };
            file.verify(text.as_bytes()).unwrap();
            assert_eq!(mime_type.as_deref(), Some("text/markdown"));
            assert!(text.starts_with("---\nname: "), "{}", file.uri);
        }
        let got = client
            .get_skill(GetSkillRequestParams::new(skill.uri.clone()))
            .await
            .unwrap();
        assert_eq!(&got.skill, skill);
    }

    let unknown = client
        .get_skill(GetSkillRequestParams::new("skill://teleport/SKILL.md"))
        .await
        .unwrap_err();
    assert_eq!(
        mcp_code(&unknown),
        Some(ErrorCode::INVALID_PARAMS),
        "{unknown}"
    );
    // A skill file that is not served is invalid params too (SEP-2640).
    let missing = client
        .read_resource(ReadResourceRequestParams::new(
            "skill://build-level/missing.md",
        ))
        .await
        .unwrap_err();
    assert_eq!(
        mcp_code(&missing),
        Some(ErrorCode::INVALID_PARAMS),
        "{missing}"
    );
    assert!(missing.to_string().contains("skill://build-level/SKILL.md"));

    let resources = client.list_all_resources().await.unwrap();
    for name in SKILLS {
        let uri = format!("skill://{name}/SKILL.md");
        let listed = resources.iter().find(|r| r.uri == uri).expect("listed");
        assert_eq!(listed.name, *name);
        assert_eq!(listed.mime_type.as_deref(), Some("text/markdown"));
    }
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn prompts_are_generated_from_the_skills() {
    let client = connect(Workflows::default()).await;
    let skills = client.list_all_skills().await.unwrap();
    let prompts = client.list_all_prompts().await.unwrap();
    let names: BTreeSet<&str> = prompts.iter().map(|p| p.name.as_str()).collect();
    assert_eq!(names, SKILLS.iter().copied().collect());
    for prompt in &prompts {
        let skill = skills
            .iter()
            .find(|s| s.frontmatter.name == prompt.name)
            .unwrap();
        assert_eq!(
            prompt.description.as_deref(),
            Some(skill.frontmatter.description.as_str())
        );
    }

    let rendered = client
        .get_prompt(
            GetPromptRequestParams::new("playtest").with_arguments(
                json!({"focus": "the coin pickups"})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .unwrap();
    let text = &rendered.messages[0].content.as_text().unwrap().text;
    assert!(
        text.contains("Pay most attention to: the coin pickups"),
        "{text}"
    );
    assert!(text.contains("# Playtest a level"), "{text}");
    assert!(
        !text.contains("name: playtest"),
        "frontmatter is not part of the prompt"
    );

    let missing = client
        .get_prompt(GetPromptRequestParams::new("build-level"))
        .await
        .unwrap_err();
    assert_eq!(
        mcp_code(&missing),
        Some(ErrorCode::INVALID_PARAMS),
        "{missing}"
    );
    assert!(missing.to_string().contains("description"), "{missing}");
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn workflows_can_be_turned_off() {
    let client = connect(Workflows {
        skills: false,
        prompts: false,
    })
    .await;
    let info = client.peer_info().expect("server info");
    assert!(!info.capabilities.supports_skills());
    assert!(info.capabilities.prompts.is_none());
    let instructions = info.instructions.as_deref().unwrap();
    assert!(instructions.contains("`build-level`") && !instructions.contains("skill://"));

    let resources = client.list_all_resources().await.unwrap();
    assert!(resources.iter().all(|r| !r.uri.starts_with("skill://")));
    let read = client
        .read_resource(ReadResourceRequestParams::new(
            "skill://build-level/SKILL.md",
        ))
        .await
        .unwrap_err();
    assert!(read.to_string().contains("unknown skill file"), "{read}");
    assert_eq!(mcp_code(&read), Some(ErrorCode::INVALID_PARAMS), "{read}");
    client.cancel().await.unwrap();

    // Prompts alone, for clients without skills that still want them.
    let client = connect(Workflows {
        skills: false,
        prompts: true,
    })
    .await;
    assert_eq!(client.list_all_prompts().await.unwrap().len(), SKILLS.len());
    client.cancel().await.unwrap();
}
