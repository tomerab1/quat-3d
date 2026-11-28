---
title: Tune the look
arguments:
  - name: goal
    description: The look to reach, e.g. "warm late-afternoon light, less washed out" or "glossier car paint".
  - name: entity
    description: An entity (id or name) whose material matters most, if any.
---
Tune how the scene in the running quat-3d engine looks, using the quat-mcp tools.
{%- if goal %}

The goal: {{ goal }}
{%- endif %}
{%- if entity %}

Material to focus on: {{ entity }}
{%- endif %}

{{ skill }}
