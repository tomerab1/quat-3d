---
title: Author a material
arguments:
  - name: goal
    description: The material to make, e.g. "weathered copper" or "frosted green glass".
    required: true
  - name: entity
    description: The entity (id or name) whose surface to author, if known.
---
Make a surface in the running quat-3d scene look like this, using the quat-mcp tools: {{ goal }}
{%- if entity %}

The surface: {{ entity }}
{%- endif %}

{{ skill }}
