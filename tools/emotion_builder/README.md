# Compatibility entry points

The PC algorithms are maintained only in `../emotion_tool_dev/`.
The four Python files here forward to that trusted local source, preserving
legacy command-line paths and imports without another copy of the algorithms.
Do not distribute this directory on its own. Keep `../product_contracts/` too.

New integrations should use `tools/emotion_tool_dev/` directly. The existing
batch files continue to call these compatibility entry points.
