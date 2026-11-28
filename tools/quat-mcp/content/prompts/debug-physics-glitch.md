---
title: Debug a physics glitch
arguments:
  - name: problem
    description: The symptom, e.g. "the crates fall through the floor" or "the barrel jitters at start".
  - name: entity
    description: An entity (id or name) involved, if known.
---
Find the cause of a physics glitch in the running quat-3d game, using the quat-mcp tools.
{%- if problem %}

The symptom: {{ problem }}
{%- endif %}
{%- if entity %}

Start with this entity: {{ entity }}
{%- endif %}

{{ skill }}
