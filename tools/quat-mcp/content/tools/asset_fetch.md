Download a Poly Haven model (an `id` from `asset_search`) into the project so it can be spawned: the glTF and every file it includes go to `assets/meshes/polyhaven/<id>/`, with `LICENSE.md` (CC0, authors, source) and `polyhaven.json` (provenance: source URLs, sizes, checksums). Every file is checked against the size and checksum Poly Haven publishes, downloads come only over https from Poly Haven's download host, paths from the API and the files the glTF references are confined to that folder, and the whole download is capped in size (pick a lower `resolution` when it is too big).

Returns `{id, name, path, gltf, files, bytes, license, source_url, authors, alpha_modes, already_present, cooked?, cook_note?}`. Spawn it with `asset_spawn_model {"path": <path>}` (or use the path as an `instanced_mesh` `gltf` for many copies), then `world_place_on_ground` to set it down.

- `resolution`: texture size, "1k" (default) is right for nearly everything; "2k" / "4k" only for close-up hero assets.
- `alpha_modes`: material counts per glTF alpha mode. Foliage usually uses MASK (alpha-tested leaves); BLEND materials render as transparent and are costlier.
- An existing download is reused (`already_present: true`) unless `overwrite` is true.
- When the engine's `quat-cook` cooker is installed (on PATH), the model is also cooked into a compressed `<id>.glb` next to the glTF and `path` points at it; `cook_note` explains when it was not.

Needs internet access to api.polyhaven.com and dl.polyhaven.org; the error says so when they are unreachable.
