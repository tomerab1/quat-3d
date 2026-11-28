# {{ name }}

Downloaded from {{ source }}: {{ source_url }}

License: [{{ license }}]({{ license_url }}) (public domain dedication). You may use, modify and redistribute this model, commercially or not, without asking permission; attribution is not required but appreciated.

Authors:
{% for author, role in authors|items %}
- {{ author }} ({{ role }})
{%- endfor %}

Resolution {{ resolution }}, fetched {{ fetched_at }} by quat-mcp `asset_fetch`. `polyhaven.json` lists every file with its source URL and checksum.
