Search Poly Haven (polyhaven.com) for free CC0 3D models to use in the world: trees, plants, rocks, furniture, props. Returns `{matches, hits: [{id, name, categories, tags, authors, polycount, downloads, download_bytes: {"1k": bytes, "2k": ...}}]}`, most downloaded first. Every word of `query` must appear in the model's id, name, tags or categories; `category` narrows further. When nothing matches, `categories` lists the categories that exist.

Pick models whose `polycount` fits the scene (thousands of instances need a few thousand triangles each; hero props can be heavier) and whose download is reasonable at 1k. Then call `asset_fetch` with the `id`.

Only models already in the project can be spawned: `asset_list_models` lists those, so check it first; there is no need to download a model twice. Needs internet access to api.polyhaven.com; the error says so when it is unreachable.
