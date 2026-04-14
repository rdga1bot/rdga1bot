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

# ── Автооновлення identity ────────────────────────────────
# STATE_LINE: якщо label автогенерований (snapshot-*) — беремо останній git commit,
# інакше — сам label (переданий аргумент).
if [[ "$LABEL" == snapshot-* ]]; then
    STATE_LINE=$(git -C ~/l2bot/rdga1bot log -1 --format="%s" 2>/dev/null)
else
    STATE_LINE="$LABEL"
fi

# Поточна git гілка і хеш для додаткового контексту
GIT_INFO=$(git -C ~/l2bot/rdga1bot log -1 --format="%h %ad" --date=short 2>/dev/null)

cat > ~/.mempalace/identity.txt << IDENTITY
Проект: rdga1bot — C++ бот для Lineage 2, ElmoreLab Kamael/Lionna.
Персонаж: ManyaTheBond (Treasure Hunter/Dagger).
Середовище: Arch Linux (CachyOS), X11, Flatpak Lutris, GE-Proton, Wine.
Репо: ~/l2bot/rdga1bot/ | Git: ${GIT_INFO}
Поточний стан: ${STATE_LINE}
Архітектура: BotBehaviorTree єдиний планувальник. RL [Learning] Enabled=true.
HP читання (MR43): render_node+0x58 → game_obj → +0x14 = HP (uint32).
Ключові файли: src/BotBehaviorTree.cpp, src/Brain.cpp, src/BehaviorTree.cpp, src/offset_scanner.cpp.
IDENTITY

echo "[MemPalace] identity оновлено: ${STATE_LINE}"
