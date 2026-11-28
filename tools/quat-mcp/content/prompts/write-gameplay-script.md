---
title: Write a gameplay script
arguments:
  - name: goal
    description: What the script should do, e.g. "coins that spin and add to a score when the player touches them".
    required: true
  - name: entity
    description: The entity (id or name) to attach the script to, if it exists already.
  - name: path
    description: File to write, relative to assets/scripts/, e.g. "coins/coin.lua".
---
Write gameplay for the running quat-3d game as a Lua script, using the quat-mcp tools: {{ goal }}
{%- if entity %}

Attach it to this entity: {{ entity }}
{%- endif %}
{%- if path %}

Save it as `assets/scripts/{{ path }}`.
{%- endif %}

{{ skill }}
