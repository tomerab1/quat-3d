Create one entity with optional name, parent and components; returns `{id, name}`. To create many entities use `scene_batch`.

Example:

```json
{"name": "Crate",
 "components": {
   "transform": {"position": [0, 0.5, 0]},
   "primitive": {"shape": "cube", "color": "#8b5a2b", "roughness": 0.8},
   "collider": {"shape": "box", "half_extents": [0.5, 0.5, 0.5]},
   "rigidbody": {"motion": "dynamic", "mass": 20}}}
```
