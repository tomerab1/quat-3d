---
title: Debug session
arguments:
  - name: problem
    description: The symptom to diagnose, e.g. "NPCs stand still" or "the frame rate drops near the lake".
  - name: entity
    description: An entity (id or name) to start from, if one is involved.
---
Diagnose a problem in the running quat-3d engine using the quat-mcp debugging tools.
{%- if problem %}

The problem: {{ problem }}
{%- endif %}
{%- if entity %}

Start with this entity: {{ entity }}
{%- endif %}

{{ skill }}
