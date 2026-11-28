---
title: Author a material
arguments:
  - name: goal
    description: The material or look to make, e.g. "weathered copper", "frosted green glass" or "glowing lava with slowly flowing cracks".
    required: true
  - name: entity
    description: The entity (id or name) whose surface to author, if known.
---
Make a surface in the running quat-3d scene look like this, using the quat-mcp tools: {{ goal }}
{%- if entity %}

The surface: {{ entity }}
{%- endif %}

{{ skill }}
