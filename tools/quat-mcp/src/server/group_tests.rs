//! The tool groups (ROADMAP S9): every `src/server/<area>_tools.rs` file is a
//! declared group, and each group contributes tools of its own.

use std::collections::BTreeSet;

use super::QuatServer;

#[test]
fn every_tools_file_is_a_declared_group() {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/server");
    let files: BTreeSet<String> = std::fs::read_dir(&dir)
        .expect("src/server is readable")
        .filter_map(|entry| {
            let name = entry.ok()?.file_name().into_string().ok()?;
            name.strip_suffix("_tools.rs")
                .map(|stem| format!("{stem}_tools"))
        })
        .collect();
    let groups: BTreeSet<String> = QuatServer::tool_groups()
        .into_iter()
        .map(|(name, _)| name.to_owned())
        .collect();
    assert_eq!(
        files, groups,
        "declare each <area>_tools.rs in tool_groups!"
    );
}

#[test]
fn groups_contribute_distinct_tools_and_merge_into_the_router() {
    let mut seen = BTreeSet::new();
    for (group, router) in QuatServer::tool_groups() {
        let tools = router.list_all();
        assert!(!tools.is_empty(), "{group} has no tools");
        for tool in tools {
            assert!(
                seen.insert(tool.name.to_string()),
                "{} is declared twice",
                tool.name
            );
        }
    }
    let merged: BTreeSet<String> = QuatServer::tools()
        .list_all()
        .into_iter()
        .map(|t| t.name.to_string())
        .collect();
    assert_eq!(merged, seen);
}
