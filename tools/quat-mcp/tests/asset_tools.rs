//! `asset_search` / `asset_fetch` through the MCP server against a local
//! stand-in for the Poly Haven API (plain HTTP on loopback): no internet.

mod common;

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use quat_mcp::server::AssetConfig;
use quat_mcp::{EngineClient, EngineConfig, QuatServer};
use rmcp::model::{CallToolRequestParams, CallToolResult};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{Value, json};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

/// Serves the routes `make_routes(base_url)` returns (path + query → body)
/// over HTTP/1.1, 404 otherwise; returns the base URL.
async fn api_server(make_routes: impl FnOnce(&str) -> HashMap<String, Vec<u8>>) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let base = format!("http://{}", listener.local_addr().unwrap());
    let routes = Arc::new(make_routes(&base));
    tokio::spawn(async move {
        loop {
            let Ok((mut sock, _)) = listener.accept().await else {
                return;
            };
            let routes = Arc::clone(&routes);
            tokio::spawn(async move {
                let mut buf = vec![0u8; 8192];
                let mut len = 0;
                while !buf[..len].windows(4).any(|w| w == b"\r\n\r\n") && len < buf.len() {
                    match sock.read(&mut buf[len..]).await {
                        Ok(0) | Err(_) => return,
                        Ok(n) => len += n,
                    }
                }
                let request = String::from_utf8_lossy(&buf[..len]);
                let target = request.split_whitespace().nth(1).unwrap_or("").to_owned();
                let (status, body) = routes
                    .get(&target)
                    .map_or(("404 Not Found", b"{}".to_vec()), |b| ("200 OK", b.clone()));
                let head = format!(
                    "HTTP/1.1 {status}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                );
                let _ = sock.write_all(head.as_bytes()).await;
                let _ = sock.write_all(&body).await;
            });
        }
    });
    base
}

async fn connect(assets: AssetConfig) -> RunningService<RoleClient, ()> {
    let mut cfg = EngineConfig::new("127.0.0.1:9");
    cfg.timeout = Duration::from_secs(2);
    let server = QuatServer::new(Arc::new(EngineClient::new(cfg)), Duration::from_secs(5))
        .with_assets(assets);
    let (server_io, client_io) = tokio::io::duplex(1 << 20);
    tokio::spawn(async move {
        let running = server.serve(server_io).await.expect("server starts");
        let _ = running.waiting().await;
    });
    common::client(client_io).await
}

async fn call(client: &RunningService<RoleClient, ()>, tool: &str, args: Value) -> CallToolResult {
    let mut req = CallToolRequestParams::new(tool.to_owned());
    if let Value::Object(map) = args {
        req = req.with_arguments(map);
    }
    client.call_tool(req).await.expect("tools/call succeeds")
}

fn text(result: &CallToolResult) -> String {
    result
        .content
        .iter()
        .filter_map(|c| c.as_text().map(|t| t.text.clone()))
        .collect::<Vec<_>>()
        .join("\n")
}

fn config(root: &std::path::Path, api: String) -> AssetConfig {
    AssetConfig {
        project_root: root.to_path_buf(),
        api,
        max_bytes: 1 << 20,
        timeout: Duration::from_secs(5),
    }
}

#[tokio::test]
async fn search_and_fetch_through_the_server() {
    let gltf = br#"{"asset":{"version":"2.0"},"materials":[{"alphaMode":"MASK"}]}"#.to_vec();
    let bin = b"0123456789".to_vec();
    let (g, b) = (gltf.clone(), bin.clone());
    let base = api_server(move |base| {
        let files = json!({"gltf": {"1k": {"gltf": {
            "url": format!("{base}/dl/rock_01_1k.gltf"), "size": g.len(),
            "include": {"rock_01.bin": {"url": format!("{base}/dl/rock_01.bin"), "size": b.len()}}
        }}}});
        HashMap::from([
            (
                "/assets?t=models".to_owned(),
                json!({"rock_01": {"name": "Rock 01", "type": 2, "tags": ["rock"],
                                   "download_count": 5, "polycount": 800}})
                .to_string()
                .into_bytes(),
            ),
            (
                "/info/rock_01".to_owned(),
                json!({"name": "Rock 01", "type": 2, "authors": {"A. Author": "All"}})
                    .to_string()
                    .into_bytes(),
            ),
            ("/files/rock_01".to_owned(), files.to_string().into_bytes()),
            ("/dl/rock_01_1k.gltf".to_owned(), g),
            ("/dl/rock_01.bin".to_owned(), b),
        ])
    })
    .await;

    let root = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(root.path().join("assets")).unwrap();
    let client = connect(config(root.path(), base)).await;

    let r = call(&client, "asset_search", json!({"query": "rock"})).await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let v: Value = serde_json::from_str(&text(&r)).unwrap();
    assert_eq!(v["hits"][0]["id"], "rock_01");
    assert_eq!(v["hits"][0]["download_bytes"]["1k"], gltf.len() + bin.len());

    let r = call(
        &client,
        "asset_fetch",
        json!({"id": "rock_01", "cook": false}),
    )
    .await;
    assert_eq!(r.is_error, Some(false), "{}", text(&r));
    let v: Value = serde_json::from_str(&text(&r)).unwrap();
    assert_eq!(v["path"], "assets/meshes/polyhaven/rock_01/rock_01_1k.gltf");
    assert_eq!(v["alpha_modes"]["MASK"], 1);
    let dir = root.path().join("assets/meshes/polyhaven/rock_01");
    assert_eq!(std::fs::read(dir.join("rock_01.bin")).unwrap(), bin);
    assert!(dir.join("LICENSE.md").is_file());

    let r = call(&client, "asset_fetch", json!({"id": "Rock 01"})).await;
    assert_eq!(r.is_error, Some(true));
    assert!(
        text(&r).contains("not a valid Poly Haven id"),
        "{}",
        text(&r)
    );
    let r = call(
        &client,
        "asset_fetch",
        json!({"id": "rock_01", "resolution": "16k"}),
    )
    .await;
    assert_eq!(
        r.is_error,
        Some(true),
        "unknown resolutions are refused locally"
    );
    client.cancel().await.unwrap();
}

#[tokio::test]
async fn unreachable_api_is_a_clear_error() {
    // Nothing listens on this port.
    let port = {
        let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        l.local_addr().unwrap().port()
    };
    let root = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(root.path().join("assets")).unwrap();
    let client = connect(config(root.path(), format!("http://127.0.0.1:{port}"))).await;
    for (tool, args) in [
        ("asset_search", json!({"query": "tree"})),
        ("asset_fetch", json!({"id": "fir_sapling"})),
    ] {
        let r = call(&client, tool, args).await;
        assert_eq!(r.is_error, Some(true));
        let t = text(&r);
        assert!(t.contains("could not reach"), "{t}");
        assert!(t.contains("asset_list_models"), "{t}");
    }
    client.cancel().await.unwrap();
}
