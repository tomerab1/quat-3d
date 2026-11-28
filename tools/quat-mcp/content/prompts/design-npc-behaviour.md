---
title: Design NPC behaviour
arguments:
  - name: goal
    description: What the NPC should do, e.g. "patrol between the gates and chase the player when near".
    required: true
  - name: entity
    description: The entity (id or name) to give the behaviour, if it already exists.
---
Design a behaviour tree in the running quat-3d engine using the quat-mcp tools. The goal: {{ goal }}
{%- if entity %}

Give the behaviour to this entity: {{ entity }}
{%- endif %}

{{ skill }}
