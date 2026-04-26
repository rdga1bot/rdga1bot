#!/bin/bash
set -euo pipefail
PALACE=~/rdga1prj/l2net/memory/palace
VENV=~/rdga1prj/l2net/memory/venv/bin/mempalace
CLAUDE_MD=~/rdga1prj/l2net/CLAUDE.md
LABEL="${1:-snapshot-$(date +%Y%m%d_%H%M%S)}"
echo "$(date -Iseconds) | $LABEL" >> ~/rdga1prj/l2net/memory/snapshots.log

CURRENT_STATE=$(grep -A6 "^## Поточний стан" "$CLAUDE_MD" \
    | grep -v "^##" | grep -v "^$" | head -6 \
    | sed 's/^- \*\*//' | sed 's/\*\*//' || true)
cat > ~/.mempalace/identity.txt << IDEOF
Проект: rdga1bot — C++ бот для Lineage 2, ElmoreLab Kamael/Lionna.
Персонаж: ManyaTheBond (Treasure Hunter/Dagger).
Середовище: Arch Linux (CachyOS), X11, Flatpak Lutris, GE-Proton, Wine.
Репо: ~/rdga1prj/l2net/
Snapshot: $LABEL
$CURRENT_STATE
IDEOF
echo "[MemPalace] Identity оновлено: $LABEL"
echo "Для повного mining: mp-l2net mine ~/rdga1prj/l2net --wing rdga1bot-src"
