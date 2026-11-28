//! The skill files and prompt templates, compiled into the binary so the
//! server needs no checkout at run time. A test keeps these lists equal to
//! the files on disk.

/// One file, by its path relative to its content directory.
#[derive(Debug, Clone, Copy)]
pub struct EmbeddedFile {
    pub path: &'static str,
    pub text: &'static str,
}

macro_rules! embed {
    ($dir:literal: $($path:literal),* $(,)?) => {
        &[$(EmbeddedFile {
            path: $path,
            text: include_str!(concat!("../../content/", $dir, "/", $path)),
        }),*]
    };
}

/// Every file under `content/skills/`, as `<skill>/<file>`.
pub const SKILL_FILES: &[EmbeddedFile] = embed!("skills":
    "author-material-shader/SKILL.md",
    "build-level/SKILL.md",
    "debug-physics-glitch/SKILL.md",
    "debug-session/SKILL.md",
    "design-npc-behaviour/SKILL.md",
    "inspect-scene/SKILL.md",
    "playtest/SKILL.md",
    "tune-look/SKILL.md",
    "write-gameplay-script/SKILL.md",
);

/// Every prompt template under `content/prompts/`, one per skill, named
/// `<skill>.md`.
pub const PROMPT_FILES: &[EmbeddedFile] = embed!("prompts":
    "author-material-shader.md",
    "build-level.md",
    "debug-physics-glitch.md",
    "debug-session.md",
    "design-npc-behaviour.md",
    "inspect-scene.md",
    "playtest.md",
    "tune-look.md",
    "write-gameplay-script.md",
);
