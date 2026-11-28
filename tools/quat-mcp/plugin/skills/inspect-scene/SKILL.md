---
name: inspect-scene
description: Survey the scene in a running quat-3d engine and report its structure, lighting, physics, gameplay components and visible problems without changing anything. Use when asked what is in a quat-3d scene, to review a level, or before editing a scene you have not seen.
compatibility: Needs the quat-mcp MCP server connected to a running quat-3d engine (started with QUAT_REMOTE).
metadata:
  quat-mcp-tools: "engine_info scene_list scene_get environment_get camera_get terrain_status screenshot"
---

# Inspect a scene

Read everything first, look at it, then report. Change nothing unless asked.

## Workflow

1. **Overview.** `engine_info`: entity count and whether play mode is
   running. The resource `quat://scene/summary` gives the same overview
   plus the first 500 entities, the environment and the camera in one read.
2. **Entities.** `scene_list`, paging with `offset` until you have seen
   `total` entities. Note the hierarchy (`parent` ids), names, components
   and `world_bounds` of each.
3. **Details.** `scene_get` on the entities that matter - lights, cameras,
   terrain, physics bodies, NPCs, scripted entities and anything related to
   the focus - to read their component values.
4. **Environment.** `environment_get` (sun and clouds), `camera_get`, and
   `terrain_status` if there is terrain.
5. **Look.** `screenshot` with `{"camera": {"frame": "<entity>"}}` for an
   overview of the main area and for anything specific; this leaves the
   user's viewport where it is.

## Report

A short description of the scene; its hierarchy; lighting (sun, point and
directional lights); physics, navigation and gameplay components; and the
problems you notice:

- unnamed or duplicate-named entities;
- objects floating above or sunk into the ground (`world_bounds` bottom
  versus the ground);
- overlapping geometry, dynamic bodies without colliders, colliders whose
  size disagrees with what is drawn;
- `nav_agent` entities in a scene that has never been baked, scripts that
  point at missing files;
- a scene too dark or blown out in the screenshot.

## Pitfalls

- `scene_list` returns at most `limit` entities per call (default 100);
  keep paging.
- A screenshot taken while terrain is still generating shows missing
  tiles; wait for `terrain_status` to report `generating: false`.
