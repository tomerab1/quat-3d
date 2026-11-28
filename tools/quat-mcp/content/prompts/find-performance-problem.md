---
title: Find a performance problem
arguments:
  - name: symptom
    description: What is slow, e.g. "stutters when the forest streams in" or "30 fps in the editor".
  - name: target_fps
    description: The frame rate to reach, e.g. "60" (the frame budget is 1000 / fps ms).
---
Find out why the running quat-3d engine is slow, using the quat-mcp profiling tools.
{%- if symptom %}

The symptom: {{ symptom }}
{%- endif %}
{%- if target_fps %}

The target: {{ target_fps }} fps. Pass a frame budget of 1000 / {{ target_fps }} ms as `budget_ms` to `profile_capture`.
{%- endif %}

{{ skill }}
