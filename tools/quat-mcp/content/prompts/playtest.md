---
title: Playtest the level
arguments:
  - name: focus
    description: What to pay most attention to, e.g. "the coin pickups" or "the guards' patrol".
---
Playtest the level in the running quat-3d engine using the quat-mcp tools and report what you find.
{%- if focus %}

Pay most attention to: {{ focus }}
{%- endif %}

{{ skill }}
