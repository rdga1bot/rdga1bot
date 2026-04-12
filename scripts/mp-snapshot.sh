#!/bin/bash
PALACE=~/l2bot/rdga1bot/memory/palace
LABEL="${1:-snapshot-$(date +%Y%m%d_%H%M%S)}"
echo "$(date -Iseconds) | $LABEL" >> ~/l2bot/rdga1bot/memory/snapshots.log

# l2bot history (MR промпти, логи, summary)
~/l2bot/rdga1bot/memory/venv/bin/mempalace --palace "$PALACE" \
    mine ~/l2bot --wing l2bot-history --mode convos

# rdga1bot docs (CLAUDE*.md, README, промпти) — без third_party
~/l2bot/rdga1bot/memory/venv/bin/mempalace --palace "$PALACE" \
    mine ~/l2bot/rdga1bot --wing rdga1bot-src

echo "[MemPalace] Snapshot збережено: $LABEL"
