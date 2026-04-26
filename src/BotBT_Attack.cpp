// BotBT_Attack.cpp
// Attack branch: actAttack, resetAttackState, resetTargetState,
// blacklistCurrentTarget.
//
// actAttack — атакує поточний таргет. Kill detection pipeline:
//   1. KnownList gs.kl_mob_died (instant, без debounce) — якщо увімкнено
//   2. HP ≤ 2% debounce 3 тіки — через Eyes::DetectTarget
//   3. NoTarget × 8 тіків — таргет зник після атаки
//   4. Watchdog таймаут (cfg.attack_watchdog секунд)
//
// resetAttackState  — скидає m_atk_* між боями.
// resetTargetState  — скидає m_tgt_* між пошуками цілі.
// blacklistCurrentTarget — блокує моба з найменшим HP на 60с.
//
// НЕ змінювати порядок перевірок kill detection (1→2→3→4).
#include "BotBehaviorTree.h"
#include "Input.h"

BTStatus BotBehaviorTree::actAttack(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Attack";

    if (!gs.hands_ready) return BTStatus::Running;

    // Screen-Y filter: перевіряємо першу атаку (таргет щойно знайдено)
    if (self.m_atk_first_attack) {
        const int kNearbyYThreshold = gs.cfg.nearby_y_threshold;
        const int kMaxFarRejects    = gs.cfg.max_far_rejects;
        if (kNearbyYThreshold > 0 && !gs.cfg.target_macro_keys.empty()
            && self.m_tgt_far_rejects < kMaxFarRejects) {
            auto npcs = gs.eyes.DetectNPCs();
            for (const auto& npc : npcs) {
                if (npc.Selected() && npc.center.y < kNearbyYThreshold) {
                    gs.log("[TARGETING] Моб далеко (cy=" + std::to_string(npc.center.y)
                        + ", reject=" + std::to_string(self.m_tgt_far_rejects + 1)
                        + "/" + std::to_string(kMaxFarRejects) + ") → ESC");
                    gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                    self.m_tgt_far_rejects++;
                    gs.hands.Send(150);
                    return BTStatus::Running; // чекаємо наступний тік (has_target=false)
                }
            }
        }
        self.m_tgt_far_rejects = 0; // прийняли таргет
    }

    // Ініціалізація watchdog при першій атаці
    if (self.m_atk_first_attack && self.m_atk_watchdog_start == TP{}) {
        self.m_atk_watchdog_start = now();
    }

    // Re-detect target bar кожні 200мс
    if (secsSince(self.m_atk_last_redetect) >= 0.2) {
        gs.eyes.ResetTarget();
        const_cast<GameState&>(gs).target = gs.eyes.DetectTarget();
        const_cast<GameState&>(gs).has_target =
            gs.target.has_value() && gs.target->hp > 0;
        self.m_atk_last_redetect = now();
    }

    // KnownList kill detection + HP з пам'яті
    if (!gs.kl_mobs.empty() && !self.m_atk_first_attack) {
        if (gs.kl_mob_died) {
            gs.log("[ATTACKING] [KnownList] Таргет мертвий → Loot");
            self.notifyKill();
            self.notifyKillRL();
            self.resetAttackState(gs);
            return BTStatus::Success; // → Selector спробує Loot
        }

        // KL-HP: вибір моба — спочатку за hp% (±15% від OCR), потім найближчий fallback.
        // Проблема "nearest": поруч може бути моб B (ближчий), а атакуємо моб A.
        // HP моба B не змінюється → hp_for_stable = const → false unreachable.
        // Рішення: пріоритет mobу чий hp% найближчий до OCR target->hp (до перезапису).
        if (gs.cfg.mem_use_for_target_hp && gs.target.has_value() && gs.coords_valid) {
            const int ocr_hp = gs.target->hp;  // OCR value before any overwrite
            float best_score  = 1e12f;
            float best_dist   = 1e12f;
            const L2Character* best      = nullptr;
            const L2Character* best_near = nullptr;  // fallback: nearest
            for (const auto& mob : gs.kl_mobs) {
                if (!mob.isAlive()) continue;
                float d = mob.distanceTo(gs.player_x, gs.player_y);
                if (d < 100.0f || d > 5000.0f) continue;  // MR77: виключаємо self (<100) та garbage coords (>5000)
                // fallback: closest by distance
                if (d < best_dist) { best_dist = d; best_near = &mob; }
                // primary: closest hp% to OCR reading (only if hpMax known)
                if (mob.hpMax > 0.f) {
                    float diff = std::abs(mob.hpPercent() - (float)ocr_hp);
                    if (diff < 15.f && diff < best_score) {
                        best_score = diff;
                        best = &mob;
                    }
                }
            }
            // hp%-match не знайшов → fallback до найближчого
            if (!best) best = best_near;
            if (best) {
                const float absHp = best->hpAbs();
                if (best->hpMax > 0.f) {
                    // Підтверджений моб з hpMax — оновлюємо OCR HP та трекер стабільності
                    float pct = best->hpPercent();
                    self.m_atk_mem_hp_valid = true;
                    self.m_atk_mem_hp_abs   = absHp;
                    const_cast<GameState&>(gs).target->hp = (int)pct;
                    if (absHp != self.m_atk_kl_hp_prev_abs) {
                        self.m_atk_kl_hp_prev_abs = absHp;
                        float d = best->distanceTo(gs.player_x, gs.player_y);
                        gs.log("[KL-HP] match dist=" + std::to_string((int)d) +
                            " hp%=" + std::to_string((int)pct) +
                            " hpAbs=" + std::to_string((int)absHp) +
                            " ocr=" + std::to_string(ocr_hp));
                    }
                } else if (absHp >= 10.f) {
                    // MR75: fallback nearest без hpMax — тільки якщо absHp реалістичний (≥10).
                    // absHp < 10 = false positive (підтверджено: hpAbs=3 при ocr=94).
                    // Не оновлюємо OCR HP (hpMax невідомий → pct ненадійний).
                    // Тільки трекер стабільності — для детекції unreachable.
                    self.m_atk_mem_hp_valid = true;
                    self.m_atk_mem_hp_abs   = absHp;
                    if (absHp != self.m_atk_kl_hp_prev_abs) {
                        self.m_atk_kl_hp_prev_abs = absHp;
                        float d = best->distanceTo(gs.player_x, gs.player_y);
                        gs.log("[KL-HP] near dist=" + std::to_string((int)d) +
                            " hpAbs=" + std::to_string((int)absHp) +
                            " ocr=" + std::to_string(ocr_hp));
                    }
                }
                // absHp < 10 && hpMax=0 → ігноруємо (false positive з регіон-скану)
            }
        }
    }

    // Kill detection: HP ≤ 2% — 3 тіки debounce
    if (gs.target.has_value() && gs.target->hp <= 2) {
        self.m_atk_hp_zero_count++;
        if (self.m_atk_hp_zero_count >= 3) {
            if (self.m_atk_first_attack) {
                gs.log("[ATTACKING] Kill(hp=0%) вже мертвий до першої атаки → Target");
                self.resetAttackState(gs);
                return BTStatus::Failure; // → Selector → Target
            }
            gs.log("[ATTACKING] Kill(hp≤2%) → Loot");
            self.notifyKill();
            self.notifyKillRL();
            self.resetAttackState(gs);
            return BTStatus::Success; // → Selector → Loot
        }
    } else {
        self.m_atk_hp_zero_count = 0;
    }

    // Таргет зник: 8 тіків debounce
    if (!gs.has_target) {
        self.m_atk_no_target_count++;
        if (self.m_atk_no_target_count == 1) {
            std::string hp_str = gs.target.has_value()
                ? std::to_string(gs.target->hp) + "%" : "?";
            gs.log("[ATTACKING] Таргет зник (hp=" + hp_str + ", no_target ×1..8)");
        }
        if (self.m_atk_no_target_count >= 8) {
            if (self.m_atk_first_attack) {
                gs.log("[ATTACKING] NoTarget ×8 (first_attack) → Target");
                self.resetAttackState(gs);
                return BTStatus::Failure;
            }
            gs.log("[ATTACKING] NoTarget ×8 → Loot");
            self.notifyKill();
            self.notifyKillRL();
            self.resetAttackState(gs);
            return BTStatus::Success;
        }
        gs.hands.Send(100);
        return BTStatus::Running;
    }
    self.m_atk_no_target_count = 0;

    // HP-stable: якщо HP не змінився 5с після першої атаки — моб недосяжний
    if (!self.m_atk_first_attack && gs.target.has_value() && gs.target->hp > 0) {
        int hp_for_stable = (self.m_atk_mem_hp_valid && self.m_atk_mem_hp_abs >= 0.f)
                            ? (int)self.m_atk_mem_hp_abs
                            : gs.target->hp;
        if (hp_for_stable != self.m_atk_last_hp) {
            self.m_atk_last_hp        = hp_for_stable;
            self.m_atk_hp_stable_since = now();
        } else if (secsSince(self.m_atk_hp_stable_since) > 5.0) {
            self.m_atk_unreachable_streak++;
            gs.log("[ATTACKING] HP стабільний 5с (моб недосяжний) → Target [unreachable] ×"
                + std::to_string(self.m_atk_unreachable_streak));
            self.blacklistCurrentTarget(gs);
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Send(200);
            self.m_unreachable_flag = true;
            self.resetAttackState(gs);
            return BTStatus::Failure; // → Selector → Target
        }
    }

    // Watchdog
    if (secsSince(self.m_atk_watchdog_start) > gs.cfg.attack_watchdog) {
        gs.log("[ATTACKING] Watchdog: таймаут → Loot");
        self.notifyKill();
        self.notifyKillRL();
        self.resetAttackState(gs);
        return BTStatus::Success;
    }

    // Атака з кулдауном
    double eff_delay = (gs.cfg.delays.enabled && self.m_atk_rd)
        ? (double)self.m_atk_rd->Get() / 1000.0
        : gs.cfg.GetAttackDelay(self.m_atk_attack_idx);

    if (secsSince(self.m_atk_last_attack) >= eff_delay) {
        if (self.m_atk_first_attack && gs.cfg.IsSpoiler()) {
            gs.hands.PressKeyboardKey(gs.cfg.spoil_key);
            gs.hands.Delay(200);
        }
        gs.hands.AttackSkill(self.m_atk_attack_idx++);
        gs.hands.Send(50);
        self.m_atk_last_attack  = now();
        self.m_atk_first_attack = false;
        gs.stats.RecordAttack();
    }

    return BTStatus::Running;
}

void BotBehaviorTree::resetAttackState(GameState& gs) {
    m_atk_first_attack        = true;
    m_atk_attack_idx          = 0;
    m_atk_no_target_count     = 0;
    m_atk_hp_zero_count       = 0;
    m_atk_last_hp             = -1;
    m_atk_mem_hp_valid        = false;
    m_atk_mem_hp_abs          = -1.f;
    m_atk_low_hp_timer_active = false;
    m_atk_watchdog_start      = TP{};  // скидаємо, щоб ініціалізувалось в actAttack
    m_atk_last_attack         = now() - std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(gs.cfg.attack_wait + 0.1));
    m_atk_last_redetect       = now();
    m_tgt_active              = false; // сигнал actTarget: новий цикл таргетингу
    if (gs.cfg.delays.enabled)
        m_atk_rd = std::make_unique<RandomDelay>(
            gs.cfg.delays.attack_mean_ms, gs.cfg.delays.attack_std_ms);
    else
        m_atk_rd.reset();
}

void BotBehaviorTree::resetTargetState(GameState& gs) {
    m_tgt_macro_attempts       = 0;
    m_tgt_step_count           = 0;
    // m_tgt_macro_idx НЕ скидаємо — зберігається між циклами
    m_tgt_minimap_rotate_count = 0;
    m_tgt_prev_dx              = 0;
    m_tgt_dx_stuck_count       = 0;
    m_tgt_far_rejects          = 0;
    m_tgt_pokemon_fired        = false;
    m_tgt_pokemon_targeted     = false;
    m_tgt_dead_esc_count       = 0;
    m_tgt_dead_cycles_total    = 0;
    m_tgt_walk_stuck_count     = 0;
    m_tgt_nav_prev_was_walk    = false;
    m_tgt_nav_stuck_recoveries = 0;
    m_tgt_last_walk_time       = now();
    m_tgt_patrol_step_idx      = 0;
    m_tgt_running_to_mob       = false;
    m_tgt_run_started          = TP{};
    m_tgt_not_ready_count      = 0;
    m_tgt_geo_path_ready       = false;
    m_tgt_geo_path_idx         = 0;
    m_tgt_pending_path         = std::nullopt;
    m_backtracking             = false;

    // Обробляємо unreachable прапор з Attack
    if (m_unreachable_flag) {
        m_attack_was_unreachable = true;
        if (!gs.cfg.target_macro_keys.empty())
            m_tgt_macro_idx = (m_tgt_macro_idx + 1) %
                              (int)gs.cfg.target_macro_keys.size();
        m_unreachable_flag = false;
    }

    if (gs.cfg.delays.enabled) {
        m_tgt_rd_rotate = std::make_unique<RandomDelay>(
            gs.cfg.delays.rotate_mean_ms, gs.cfg.delays.rotate_std_ms);
        m_tgt_rd_walk = std::make_unique<RandomDelay>(
            gs.cfg.delays.walk_mean_ms, gs.cfg.delays.walk_std_ms);
    } else {
        m_tgt_rd_rotate.reset();
        m_tgt_rd_walk.reset();
    }
}

void BotBehaviorTree::blacklistCurrentTarget(GameState& gs) {
    if (!gs.cb.blacklist_mob || gs.kl_mobs.empty()) return;
    int   bid     = 0;
    float best_hp = 1e9f, best_pct = 101.f;
    for (const auto& mob : gs.kl_mobs) {
        if (!mob.isAlive()) continue;
        if (mob.hpMax > 0.f) {
            float p = mob.hpPercent();
            if (p < best_pct) { best_pct = p; bid = mob.objectID; }
        } else if (mob.hpAbs() < best_hp) {
            best_hp = mob.hpAbs(); bid = mob.objectID;
        }
    }
    if (bid != 0) gs.cb.blacklist_mob(bid, 60.f);
}
