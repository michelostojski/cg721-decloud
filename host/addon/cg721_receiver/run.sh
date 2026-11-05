#!/bin/sh
CONFIG=/data/options.json
PORT=$(python3 -c "import json;print(json.load(open('$CONFIG')).get('port',8099))" 2>/dev/null || echo 8099)
KEEP=$(python3 -c "import json;print(json.load(open('$CONFIG')).get('keep',300))" 2>/dev/null || echo 300)
export CLIP_DIR=/media/cg721
export CLIP_PORT="$PORT"
export CLIP_KEEP="$KEEP"
echo "[cg721_receiver] starting on :$PORT -> /media/cg721 (keep=$KEEP)"
exec python3 /clip_receiver.py
