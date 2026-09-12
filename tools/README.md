# Product tools and assets

- `emotion_tool_dev/`: canonical PC asset builder, Character Pack compiler,
  character profiles and regression tests.
- `emotion_builder/`: compatibility entry points; no separate algorithm copy.
- `product_contracts/`: shared schemas, examples and validator required by the compiler.
- `mjpeg_ai_dialogue_v5p3_mouth_focus/`: the archived six-expression SD pack.
  Firmware deployment uses `/sdcard/dialogue_v2/`; do not mix files from different packs.
- `code_health/`: architecture, image-layout and public-archive checks.
- `host_tests/`, `validation/`: automated production-oriented tests and gates.
- Other reminder/MCP and development utilities retain their own usage guides.

Historical asset directories are retained as references, not alternative default
deployments. The archived pack is not proof of the contents of a currently inserted
SD card. A source archive is not a tested firmware binary; see
[`../docs/source-archive.md`](../docs/source-archive.md).

From the repository root:

```sh
python -m unittest discover -s tools/emotion_tool_dev -p test_character_pack_compiler.py -v
python tools/emotion_tool_dev/dialogue_pack_contract.py tools/mjpeg_ai_dialogue_v5p3_mouth_focus
python tools/code_health/public_archive_guard.py
```

Keep game projects, local rules/AGENTS, device backups, private settings, build
outputs and runtime logs outside the public archive. The archive guard checks
the Git index and therefore must run after staging, not merely before copying.
