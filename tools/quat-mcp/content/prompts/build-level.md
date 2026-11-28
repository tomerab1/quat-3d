---
title: Build a level
arguments:
  - name: description
    description: What to build, e.g. "a small medieval village at sunset".
    required: true
  - name: constraints
    description: Extra constraints, e.g. "keep the existing terrain" or "under 200 entities".
---
Build this in the running quat-3d engine using the quat-mcp tools: {{ description }}
{%- if constraints %}

Constraints: {{ constraints }}
{%- endif %}

{{ skill }}
