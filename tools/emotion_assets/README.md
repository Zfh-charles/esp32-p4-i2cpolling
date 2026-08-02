# Emotion assets (dialogue_v2)

## Recommended baseline

Use **`dialogue_v2_canonical/`** as the current release pack.

- Device path: copy pack contents to `/sdcard/dialogue_v2/`
- Contains JPEG frames, RGB565 hold/mouth/eye layers, manifests
- Do **not** use the older root folder `mjpeg_ai_dialogue_v2` (incomplete RGB565/eyes)

## Experimental

See [`../experimental/dialogue_v2_posebank/`](../experimental/dialogue_v2_posebank/) — pose_1 / blink bank.
Firmware currently rolls back blink/pose choreography (`s1da`); do not treat posebank as the shipping baseline.
