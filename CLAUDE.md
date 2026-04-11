# rdga1bot — Контекст для Claude Code

## Середовище
- OS: Arch Linux (CachyOS), X11, користувач rdga1
- Гра: Lineage 2, ElmoreLab (Kamael/Lionna), персонаж ManyaTheBond (Treasure Hunter/Dagger)
- Клієнт: Flatpak Lutris + GE-Proton/Wine
- Папка: ~/l2bot/rdga1bot/
- Build: bash build.sh && ./launch.sh

## Поточний стан
- MR20a/b/c виконано — BotBehaviorTree (Stackless VM, ~9KB, 0 heap, ~2µs/тік)
- A/B switch: [BehaviorTree] Enabled=true → BotBehaviorTree (активний)
- farm_objectives.h + objective_manager.* — ще присутні, НЕ видалено
- Наступні пріоритети: тест BT фарму → MR20c cleanup → розбивка actTarget

## Критичні правила (НІКОЛИ не порушувати)
- W/S/A/D — НЕ використовувати (відкривають чат L2), рух тільки стрілками
- UseForKillDetect=false НАЗАВЖДИ (баг: fake kills)
- Auto-loot server-side: LOOTING = ESC+300ms (без pickup key)
- XTest для key injection (не xdotool --window)
- ScrollLock = зупинка бота
- Нові фічі: завжди feature flag в INI (Enabled=false за замовчуванням)
- Мінімальні цільові зміни — не робити broad rewrites

## Ключові файли
- src/Brain.cpp             — координатор (сприйняття + потіони + dispatch)
- src/BotBehaviorTree.h/.cpp — Farm BT (активний планувальник)
- src/BehaviorTree.h/.cpp   — Stackless VM
- src/game_state.h          — GameState struct
- src/farm_objectives.h     — ObjectiveManager objectives (legacy)
- src/offsets_config.h      — ElmoreLab Kamael offsets (відкалібровано)
- build.sh                  — компіляція (g++, без cmake)
- rdga1bot.example.ini      — всі опції з коментарями

## НЕ включати в білд
Runloop.cpp, Options.cpp — legacy від l2cvbot

## MemPalace — контекст між сесіями
На початку кожної сесії:
  mp --palace ~/l2bot/rdga1bot/memory/palace wake-up
Перед кожним MR:
  ./scripts/mp-snapshot.sh "опис змін"

## Детальний контекст (читай за потребою)
- Архітектура + BT дерево + Config INI: cat CLAUDE_ARCH.md
- Історія MR1-MR20c:                   cat CLAUDE_HISTORY.md
- Memory offsets + технічні факти:     cat CLAUDE_OFFSETS.md
