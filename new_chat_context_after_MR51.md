# rdga1bot — контекст для Claude Code (2026-04-16, після MR51)

## Проект
C++ бот для Lineage 2, ElmoreLab Kamael/Lionna. Персонаж: ManyaTheBond (Treasure Hunter/Dagger).
Середовище: Arch Linux (CachyOS), X11, Flatpak Lutris, GE-Proton, Wine.
Репо: /home/rdga1/rdga1prj/l2net/ | Build: bash build.sh && ./launch.sh

## Поточний стан: MR51 виконано (2026-04-16)

### Архітектура (незмінна з MR20c)
- BotBehaviorTree — єдиний планувальник
- BehaviorTree.h/.cpp — stackless VM (BTNode 24B, BTState 8B)
- Brain.cpp — координатор (сприйняття + потіони + dispatch)
- RL: LinearQModel, LearningWorker, [Learning] Enabled=true за замовчуванням

### Підтверджена архітектура пам'яті (MR43)
- render_node: XY@+0x90/0x94, ptr→game_obj @+0x58
- game_obj: vtable@+0x00, HP(uint32)@+0x14, state@+0x1c, ptr→render_node @+0x44
- HP читається як uint32: render_node+0x58 → game_obj → +0x14

### Виконані MR (хронологія)
- MR26-44: MemoryValidator, BT піддерево, RL, HP fix, hardening (див. CLAUDE_HISTORY.md)
- MR46b: зупинка атаки по dist; kClosePx 35→70px
- MR47: m_buff_after_death — форс-баф після respawn (ігнорує minimap_close_threat)
- MR48: KL-HP nearest-mob (не min-HP); [KL-HP] логування; видалено false has_target override
- MR49: QA stats фільтрація по сесії; kl_hp парсер; anomaly_engine kl_hp детектори
- MR50: m_atk_unreachable_streak — після 5 unreachable без kills → форсуємо повний цикл
- MR51: m_atk_streak_force_count — після 3 force-циклів (~75с) → ESC + has_target=false
        QA death_loop: рахує лише "Фаза 0" (1 смерть = 4 [DEAD] рядки → fix false CRITICAL)

### Результати live тестування (2026-04-16)
- Сесія 144928 (MR50): kills=381, deaths=1, 125хв, rate=3.4/хв, 1 gap 20хв
- Сесія 185431 (MR51): kills=37, deaths=0, 10хв, rate=3.5/хв, **0 gaps** ✓
- Ціль kill rate > 3/хв — досягнуто

### Ключові INI параметри
- [Learning] Enabled=true (RL активовано)
- [Memory] UseForTargetHP=true (KL-HP tracking)
- UseForKillDetect=false НАЗАВЖДИ

### Наступні пріоритети
- Продовжувати live farm; спостерігати streak/force у логах
- Kill rate > 3/хв стабільно через довгі сесії (1+ год)

## Критичні правила (НІКОЛИ не порушувати)
- W/S/A/D — НЕ використовувати (відкривають чат L2), тільки стрілки
- UseForKillDetect=false НАЗАВЖДИ (баг: fake kills)
- Auto-loot server-side: LOOTING = ESC+300ms (без pickup key)
- BT tick() сигнатура: std::string tick(GameState& gs) — не міняти
- m_children[] — не переставляти під час виконання
- Нові фічі: feature flag в INI, Enabled=false за замовчуванням
- Мінімальні зміни — не робити broad rewrites
