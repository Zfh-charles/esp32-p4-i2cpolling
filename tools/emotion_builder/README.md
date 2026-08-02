# emotion_builder

PC helpers that build dialogue_v2 packs (ROI / mouth / eye layers, manifests).

Source tree on the build machine: `emotion_tool_dev/`.

## Typical usage

```bat
run_dialogue_v2.bat
```

Or:

```bat
python dialogue_emotion_builder.py --help
```

Install deps: `pip install -r requirements.txt`

## Output → device

Prefer publishing into `tools/emotion_assets/dialogue_v2_canonical/`, then copy that
tree to the device as `/sdcard/dialogue_v2/`.
