---
title: Set the weather
arguments:
  - name: description
    description: The weather to reach, e.g. "a windy autumn afternoon after rain" or "light snowfall".
---
Set the weather of the scene in the running quat-3d engine, using the quat-mcp tools.
{%- if description %}

The weather to reach: {{ description }}
{%- endif %}

{{ skill }}
