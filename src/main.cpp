#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Windows.h>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <utility>
#include "SIPInputQueue.hpp"

using namespace geode::prelude;

// ============================================================================
//  USER SETTINGS
// ============================================================================

// True Wave Spam Guard: only bypasses the buffer if you do at least
// SPAM_STREAK_REQUIRED consecutive inputs faster than SPAM_INTERVAL_TICKS apart.
// This keeps isolated micro-clicks and spike/sawblade dodges 100% protected!
constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_STREAK_REQUIRED = 3;

// ============================================================================

class TrajectoryBufferSim {
private:
    using SIPInput = subtickinputs::PlayerButtonCommandWithRatio;
    using SIPInputQueue = std::vector<SIPInput>;

    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;
    bool m_simulationUnsupported = false;
    bool m_warnedInputModConflict = false;

    // Tracks actual physical mouse state vs what PlayLayer currently has active
    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    // F6 Cycle State (Starts on Stage 0: 2 ticks Normal / 2 ticks Wave)
    int  m_presetIndex = 0;
    bool m_enabled = true;
    int  m_maxTicksNormal = 2;
    int  m_maxTicksWave = 2;

    // Buffer state
    int  m_waitTicks = -1;          // -1 = inactive, >= 0 = countdown ticks, -2 = queued opposite tick
    bool m_bufferedPush = true;     // true = buffered PRESS, false = buffered RELEASE
    bool m_queuedOpposite = false;  // true if user quickly released/re-pressed during wait

    // SIP commands retain their sub-tick ratio when moved to a later physics tick.
    bool m_hasBufferedSIPInput = false;
    bool m_hasQueuedOppositeSIPInput = false;
    bool m_sipReadyAction = false;
    bool m_sipReadyPush = true;
    SIPInput m_bufferedSIPInput{};
    SIPInput m_queuedOppositeSIPInput{};
    float m_frameDt = 0.25f;        // GD 240 TPS normalized dt is 0.25f

    // Streak-based Spam Guard state
    int  m_ticksSinceLastInput = 999;
    int  m_rapidInputStreak = 0;

    PlayerObject* createTrajectoryPlayer() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return nullptr;

        PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
        player->retain();
        player->setPosition({0, 105});
        player->setVisible(false);
        pl->m_objectLayer->addChild(player);
        return player;
    }

    void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    // Matches Show Trajectory's exact attribute + hitbox size synchronization
    void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
        sim->setVisible(false);
        sim->copyAttributes(real);
        sim->setPosition(real->getPosition());
        sim->setRotation(real->getRotation());
        sim->m_yVelocity = real->m_yVelocity;
        sim->m_jumpBuffered = real->m_jumpBuffered;
        sim->m_isOnGround2 = real->m_isOnGround2;
        sim->m_platformerXVelocity = real->m_platformerXVelocity;
        sim->setScale(real->getScale());
        sim->setContentSize(real->getContentSize());
        sim->m_width = real->m_width;
        sim->m_height = real->m_height;
        sim->m_gravityMod = real->m_gravityMod;
        sim->m_isOnGround = real->m_isOnGround;
        sim->m_isUpsideDown = real->m_isUpsideDown;
        sim->m_vehicleSize = real->m_vehicleSize;
        sim->m_playerSpeed = real->m_playerSpeed;
    }

    void clearPendingInput() {
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_hasBufferedSIPInput = false;
        m_hasQueuedOppositeSIPInput = false;
        m_sipReadyAction = false;
    }

    void appendSIPAction(PlayLayer* pl, SIPInputQueue& output, SIPInput source, bool push,
                         std::vector<std::pair<double, bool>>* fallbackActions = nullptr) {
        source.input.m_button = PlayerButton::Jump;
        source.input.m_isPush = push;
        source.input.m_isPlayer2 = false;
        output.push_back(source);
        if (fallbackActions) fallbackActions->push_back({source.ratio, push});

        if (pl && pl->m_gameState.m_isDualMode && (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
            source.input.m_isPlayer2 = true;
            output.push_back(source);
        }
        m_gameHolding = push;
    }

    void flushPendingInput(PlayLayer* pl) {
        if (!pl) {
            clearPendingInput();
            return;
        }

        // This path is reserved for emergency passthroughs (e.g. an input-mod conflict or
        // a mode transition). Never drop a captured press/release pair in that case.
        if (m_waitTicks >= 0) {
            m_SimFireButton(pl, m_bufferedPush);
            if (m_queuedOpposite) m_SimFireButton(pl, !m_bufferedPush);
        } else if (m_waitTicks == -2 || m_sipReadyAction) {
            m_SimFireButton(pl, !m_bufferedPush);
        }
        clearPendingInput();
    }

    float getValidDt() const {
        if (m_frameDt > 0.001f && m_frameDt < 2.0f) {
            return m_frameDt;
        }
        return 0.25f;
    }

public:
    void init() {
        quit();
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;

        m_simPlayer1 = createTrajectoryPlayer();
        m_simPlayer2 = createTrajectoryPlayer();
        clearPendingInput();
        m_physicalHolding = false;
        m_gameHolding = false;
        m_simulating = false;
        m_firingBuffered = false;
        m_ticksSinceLastInput = 999;
        m_rapidInputStreak = 0;
        m_warnedInputModConflict = false;
        m_simulationUnsupported = false;
    }

    void quit() {
        // Remove both hidden simulation players from the object layer before releasing them.
        if (m_simPlayer1) {
            if (m_simPlayer1->getParent()) m_simPlayer1->removeFromParentAndCleanup(true);
            m_simPlayer1->release();
            m_simPlayer1 = nullptr;
        }
        if (m_simPlayer2) {
            if (m_simPlayer2->getParent()) m_simPlayer2->removeFromParentAndCleanup(true);
            m_simPlayer2->release();
            m_simPlayer2 = nullptr;
        }
        m_simulating = false;
        m_firingBuffered = false;
        clearPendingInput();
        m_rapidInputStreak = 0;
        m_simulationUnsupported = false;
    }

    // F9 cycles through 2, 4, 8, 16, 24 ticks, then OFF.
    // A command already in flight remains on its captured schedule when presets change.
    void cyclePreset() {
        // Leave an already-captured input on its existing schedule. The buffer scheduler
        // continues any pending action even if the selected preset becomes OFF.
        m_presetIndex = (m_presetIndex + 1) % 6;
        switch (m_presetIndex) {
            case 0:
                m_enabled = true; m_maxTicksNormal = 2; m_maxTicksWave = 2;
                printf("[InputBuffer] F9 Cycle -> STAGE 1: 2 Ticks All | 2 Ticks Wave (0.5f @ 60fps)\n"); break;
            case 1:
                m_enabled = true; m_maxTicksNormal = 4; m_maxTicksWave = 4;
                printf("[InputBuffer] F9 Cycle -> STAGE 2: 4 Ticks All | 4 Ticks Wave (1.0f @ 60fps)\n"); break;
            case 2:
                m_enabled = true; m_maxTicksNormal = 8; m_maxTicksWave = 8;
                printf("[InputBuffer] F9 Cycle -> STAGE 3: 8 Ticks All | 8 Ticks Wave (2.0f @ 60fps)\n"); break;
            case 3:
                m_enabled = true; m_maxTicksNormal = 16; m_maxTicksWave = 16;
                printf("[InputBuffer] F9 Cycle -> STAGE 4: 16 Ticks All | 16 Ticks Wave (4.0f / 4.0f @ 60fps)\n"); break;
            case 4:
                m_enabled = true; m_maxTicksNormal = 24; m_maxTicksWave = 24;
                printf("[InputBuffer] F9 Cycle -> STAGE 5: 24 Ticks All | 24 Ticks Wave (6.0f / 6.0f @ 60fps)\n"); break;
            case 5:
                m_enabled = false; m_maxTicksNormal = 0; m_maxTicksWave = 0;
                printf("[InputBuffer] F9 Cycle -> STAGE 6: OFF (100%% Raw Input)\n"); break;
        }
    }


    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }
    bool isSimulatedPlayer(PlayerObject* player) const {
        return player && (player == m_simPlayer1 || player == m_simPlayer2);
    }
    void markSimulationUnsupported(PlayerObject* player = nullptr) {
        if (m_simulating && (!player || isSimulatedPlayer(player))) {
            m_simulationUnsupported = true;
        }
    }

    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }
        // Real player died -> clear any pending buffer so respawn is 100% clean
        clearPendingInput();
        m_gameHolding = false;
        m_rapidInputStreak = 0;
        return false;
    }

    void setFrameDelta(float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && pl->m_gameState.m_timeWarp > 0.f) {
            m_frameDt = dt / pl->m_gameState.m_timeWarp;
        } else {
            m_frameDt = dt;
        }
    }

    int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual, int delayFrames, bool actionPush, int maxTicks) {
        // Missing simulation data must never be interpreted as a safe trajectory.
        if (!pl || !m_simPlayer1 || !realP1 || (isDual && (!m_simPlayer2 || !realP2))) return -1;

        m_simulating = true;
        m_simulationDead = false;
        m_simulationUnsupported = false;

        syncSimPlayer(m_simPlayer1, realP1);
        if (isDual) syncSimPlayer(m_simPlayer2, realP2);

        // Recreate the button state that precedes this hypothetical transition.
        if (delayFrames > 0) {
            if (!actionPush) {
                m_simPlayer1->pushButton(PlayerButton::Jump);
                if (isDual) m_simPlayer2->pushButton(PlayerButton::Jump);
            } else {
                m_simPlayer1->releaseButton(PlayerButton::Jump);
                if (isDual) m_simPlayer2->releaseButton(PlayerButton::Jump);
            }
        }

        float dt = getValidDt();
        int survived = 0;

        // A brief landing is not proof of full-horizon survival. Run the complete forecast.
        for (int i = 0; i < delayFrames + maxTicks; i++) {
            if (i == delayFrames) {
                if (actionPush) {
                    m_simPlayer1->pushButton(PlayerButton::Jump);
                    if (isDual) m_simPlayer2->pushButton(PlayerButton::Jump);
                } else {
                    m_simPlayer1->releaseButton(PlayerButton::Jump);
                    if (isDual) m_simPlayer2->releaseButton(PlayerButton::Jump);
                }
            }

            m_simPlayer1->update(dt);
            m_simPlayer1->updateRotation(dt);
            if (isDual) {
                m_simPlayer2->update(dt);
                m_simPlayer2->updateRotation(dt);
            }
            if (m_simulationDead || m_simulationUnsupported) break;

            resetCollisionLog(m_simPlayer1);
            pl->checkCollisions(m_simPlayer1, dt, false);
            if (m_simulationDead || m_simulationUnsupported) break;

            if (isDual) {
                resetCollisionLog(m_simPlayer2);
                pl->checkCollisions(m_simPlayer2, dt, false);
                if (m_simulationDead || m_simulationUnsupported) break;
            }

            if (i >= delayFrames) survived++;
        }

        m_simulating = false;
        return m_simulationUnsupported ? -1 : survived;
    }

    bool isSIPEnabled() const {
        auto* loader = Loader::get();
        if (!loader) return false;
        auto* sip = loader->getLoadedMod("chizz.superb-input-precision");
        return sip && sip->getSettingValue<bool>("mod-enabled");
    }

    bool isCBFLoaded() const {
        auto* loader = Loader::get();
        if (!loader) return false;
        return loader->getLoadedMod("syzzi.click_between_frames") != nullptr ||
               loader->getLoadedMod("zmx.cbf-lite") != nullptr ||
               loader->getLoadedMod("alphalaneous.click_after_frames") != nullptr;
    }

    bool hasInputModConflict() const {
        return isSIPEnabled() && isCBFLoaded();
    }

    bool usesSIPTickClock() const {
        return isSIPEnabled() && !isCBFLoaded();
    }

    bool filterSIPInputQueue(float dt) {
        if (!usesSIPTickClock() || m_simulating || dt <= 0.f) return false;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) return false;

        auto queueResult = subtickinputs::getInputQueueWithRatios(dt);
        if (queueResult.isErr()) {
            log::warn("SIP queue API unavailable; flushing any captured input and leaving raw queue untouched");
            flushPendingInput(pl);
            if (GetAsyncKeyState(VK_F9) & 1) cyclePreset();
            return false;
        }

        auto queueWithRatios = queueResult.unwrap();
        SIPInputQueue filtered;
        filtered.reserve(queueWithRatios.size() + 4);
        std::vector<std::pair<double, bool>> fallbackActions;

        struct SIPQueueEvent {
            SIPInput command;
            bool isBufferedAction;
            bool push;
        };
        std::vector<SIPQueueEvent> timeline;
        timeline.reserve(queueWithRatios.size() + 1);
        for (auto const& item : queueWithRatios) {
            timeline.push_back({item, false, item.input.m_isPush});
        }

        // Only this path owns F9 while SIP is active.
        if (GetAsyncKeyState(VK_F9) & 1) cyclePreset();

        // Merge a ready buffered command into the same chronological stream as fresh inputs.
        // This matters when a new sub-tick input occurs before the delayed command's ratio.
        if (m_sipReadyAction) {
            SIPInput source{};
            if (m_sipReadyPush == m_bufferedPush && m_hasBufferedSIPInput) {
                source = m_bufferedSIPInput;
            } else if (m_sipReadyPush != m_bufferedPush && m_hasQueuedOppositeSIPInput) {
                source = m_queuedOppositeSIPInput;
            } else if (m_hasBufferedSIPInput) {
                source = m_bufferedSIPInput;
            }
            source.input.m_button = PlayerButton::Jump;
            source.input.m_isPush = m_sipReadyPush;
            source.input.m_isPlayer2 = false;
            timeline.push_back({source, true, m_sipReadyPush});
        }

        std::stable_sort(timeline.begin(), timeline.end(), [](SIPQueueEvent const& a, SIPQueueEvent const& b) {
            return a.command.ratio < b.command.ratio;
        });

        for (auto const& event : timeline) {
            if (event.isBufferedAction) {
                appendSIPAction(pl, filtered, event.command, event.push, &fallbackActions);
                m_sipReadyAction = false;
                if (event.push == m_bufferedPush && m_queuedOpposite) {
                    // The original press/release pair stays separated by a complete physics tick.
                    m_waitTicks = -2;
                    m_queuedOpposite = false;
                } else {
                    clearPendingInput();
                }
                continue;
            }

            SIPInput item = event.command;
            bool keepInput = true;
            if (static_cast<int>(item.input.m_button) == 1) {
                keepInput = interceptInput(pl, item.input.m_isPush, !item.input.m_isPlayer2,
                                           &item, &filtered, &fallbackActions);
            }
            if (keepInput) filtered.push_back(item);
        }

        // setInputQueueWithRatios timestamps by ratio; keep commands chronologically ordered.
        std::stable_sort(filtered.begin(), filtered.end(), [](SIPInput const& a, SIPInput const& b) {
            return a.ratio < b.ratio;
        });
        // Keep our logical hold state aligned with the exact queue SIP will receive.
        bool finalGameHolding = m_gameHolding;
        for (auto const& item : filtered) {
            if (static_cast<int>(item.input.m_button) == 1 && !item.input.m_isPlayer2) {
                finalGameHolding = item.input.m_isPush;
            }
        }
        m_gameHolding = finalGameHolding;

        auto setResult = subtickinputs::setInputQueueWithRatios(std::move(filtered), dt);
        if (setResult.isErr()) {
            log::warn("SIP rejected the filtered queue; replaying already-due buffered actions and passing this tick's raw inputs through");
            std::stable_sort(fallbackActions.begin(), fallbackActions.end(),
                [](auto const& a, auto const& b) { return a.first < b.first; });
            for (auto const& action : fallbackActions) m_SimFireButton(pl, action.second);
            clearPendingInput();
            m_rapidInputStreak = 0;
            for (auto const& item : queueWithRatios) {
                if (static_cast<int>(item.input.m_button) == 1 && !item.input.m_isPlayer2) {
                    m_physicalHolding = item.input.m_isPush;
                }
            }
            m_gameHolding = m_physicalHolding;
            return false;
        }
        return true;
    }

    void onSIPPhysicsTick() {
        if (!usesSIPTickClock()) return;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1 ||
            (!m_enabled && m_waitTicks == -1 && !m_sipReadyAction)) return;

        if (m_ticksSinceLastInput < 999) m_ticksSinceLastInput++;

        // SIP processes the queue before processCommands. Mark an expiring action ready here;
        // it is injected at its preserved ratio in the next queue-processing phase.
        if (pl->m_player1->m_isShip ||
            (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip)) {
            flushPendingInput(pl);
            if (m_gameHolding != m_physicalHolding) m_SimFireButton(pl, m_physicalHolding);
            return;
        }

        if (m_waitTicks > 0) {
            --m_waitTicks;
            if (m_waitTicks == 0) {
                m_sipReadyAction = true;
                m_sipReadyPush = m_bufferedPush;
            }
        } else if (m_waitTicks == -2) {
            m_sipReadyAction = true;
            m_sipReadyPush = !m_bufferedPush;
        } else if (m_waitTicks == -1 && m_gameHolding != m_physicalHolding) {
            m_SimFireButton(pl, m_physicalHolding);
        }
    }

    bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1,
                        SIPInput const* sipInput = nullptr, SIPInputQueue* sipOutput = nullptr,
                        std::vector<std::pair<double, bool>>* fallbackActions = nullptr) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;
        // The trajectory clone mirrors both players only in shared-control Dual mode.
        // In 2-player-control levels, player 1's input must not be simulated on player 2.
        bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr && !isTwoPlayerControl;

        // Keep Player 2 synced with Player 1 in 1-player Dual mode
        if (!isPlayer1) {
            if (isDual && !isTwoPlayerControl && (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        // Always record the user's true physical mouse/key state first!
        m_physicalHolding = push;

        if (hasInputModConflict()) {
            flushPendingInput(pl);
            m_gameHolding = push;
            if (!m_warnedInputModConflict) {
                printf("[InputBuffer] WARNING: CBF and SIP are both active. Disable one; buffer bypassed.\n");
                m_warnedInputModConflict = true;
            }
            return true;
        }

        if (!m_enabled && m_waitTicks == -1 && !m_sipReadyAction) {
            m_gameHolding = push;
            return true;
        }

        PlayerObject* p1 = layer->m_player1;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        if (!p1 || p1->m_isDead) {
            clearPendingInput();
            m_gameHolding = push;
            return true;
        }

        // 1. NEVER touch Ship mode (or Dual with Ship)
        if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
            flushPendingInput(pl);
            m_gameHolding = push;
            return true;
        }

        // 2. If an input is currently waiting in the buffer and the user quickly releases
        // (or re-presses) 1-2 ticks later, queue it for the next tick so fast taps are never eaten
        if (m_waitTicks >= 0) {
            if (push != m_bufferedPush) {
                m_queuedOpposite = true;
                if (sipInput) {
                    m_queuedOppositeSIPInput = *sipInput;
                    m_hasQueuedOppositeSIPInput = true;
                }
            } else {
                m_queuedOpposite = false;
                m_hasQueuedOppositeSIPInput = false;
            }
            m_ticksSinceLastInput = 0;
            return false;
        }

        // In SIP mode, a queued opposite command is represented by a scheduled queue event.
        // Do not fire it a second time from a later event in the same queue-processing pass.
        if (m_waitTicks == -2 && sipOutput) {
            m_gameHolding = push;
            return true;
        }

        // Native tick scheduler: if an opposite action is due and another input arrives,
        // restore the pending transition before processing the new command.
        if (m_waitTicks == -2) {
            bool fireOpposite = m_gameHolding != !m_bufferedPush;
            SIPInput queuedOpposite = m_queuedOppositeSIPInput;
            bool hasQueuedOpposite = m_hasQueuedOppositeSIPInput;
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_hasQueuedOppositeSIPInput = false;
            if (fireOpposite) {
                if (sipOutput && hasQueuedOpposite) {
                    appendSIPAction(pl, *sipOutput, queuedOpposite, !m_bufferedPush, fallbackActions);
                } else {
                    m_SimFireButton(pl, !m_bufferedPush);
                    if (fallbackActions) fallbackActions->push_back({sipInput ? sipInput->ratio : 0.0, !m_bufferedPush});
                }
            }
            m_hasBufferedSIPInput = false;
            m_sipReadyAction = false;
        }

        bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);
        bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
                          (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);
        bool isCube = p1->isInNormalMode() && (!isDual || !p2 || p2->isInNormalMode());

        // 3. STREAK-BASED WAVE SPAM GUARD:
        // Only bypasses if you perform 3+ rapid inputs in a row (< 8 ticks apart).
        // Single micro-clicks and normal Wave turns around spikes/sawblades stay 100% active!
        if (m_ticksSinceLastInput < SPAM_INTERVAL_TICKS) {
            m_rapidInputStreak++;
        } else {
            // Count this event as the first input in a fresh burst.
            m_rapidInputStreak = 1;
        }
        m_ticksSinceLastInput = 0;

        if (isWave && m_rapidInputStreak >= SPAM_STREAK_REQUIRED) {
            m_gameHolding = push;
            return true;
        }

        // Only Wave (m_isDart) and Robot (m_isRobot) change trajectory on RELEASE
        bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot ||
                                     (isDual && p2 && (p2->m_isDart || p2->m_isRobot));
        if (!push && !releaseAffectsPhysics) {
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;
        const char* act = push ? "PRESS" : "RELEASE";

        // Full Show-Trajectory lookahead for Wave (65 ticks / 50 ticks Mini Wave)
        // so it sees spikes and sawblades 1-3 blocks ahead along the diagonal path!
        int targetHorizon = isMiniWave ? 50 : (isWave ? 65 : 160);
        int maxDelayFrames = isWave ? m_maxTicksWave : m_maxTicksNormal;

        // 4. Check Frame 0 (immediate input) across the full trajectory horizon
        int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
        if (ticksFrame0 >= targetHorizon) {
            m_gameHolding = push;
            return true;
        }
        // If the path touches unsupported trigger logic, do not let a guessed trajectory
        // delay an input. Pass through the player's actual command instead.
        if (ticksFrame0 < 0) {
            m_gameHolding = push;
            return true;
        }

        // 5. Frame 0 hits a spike, sawblade, or slope within the horizon!
        // Scan +1..maxDelayFrames for the earliest delay that genuinely clears the obstacle.
        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        for (int delay = 1; delay <= maxDelayFrames; delay++) {
            int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon);

            if (isWave) {
                // Preserve LilWC's wave-specific trajectory heuristic: a delayed turn may
                // clear the immediate corner/spike even when another hazard lies later.
                int minClearedGain = (delay * 2) + 10;
                if (ticks >= targetHorizon && (ticks - ticksFrame0) >= (delay * 2) + 6) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks >= ticksFrame0 + minClearedGain && ticks > bestTicks + 2) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    if (ticks >= targetHorizon) break;
                }
            } else if (ticks >= targetHorizon) {
                // For cube/robot/ball/UFO and other modes, only count a complete safe horizon.
                bestDelay = delay;
                bestTicks = ticks;
                break;
            }
        }

        if (bestDelay > 0) {
            printf("[InputBuffer] *** SAVED +%d TICKS (%.2f frames @ 60fps) %s! (Frame0: %d -> Delayed: %d) [Limit: %d] ***\n",
                bestDelay, bestDelay / 4.0f, act, ticksFrame0, bestTicks, maxDelayFrames);
            m_bufferedPush = push;
            m_waitTicks = bestDelay;
            if (sipInput) {
                m_bufferedSIPInput = *sipInput;
                m_hasBufferedSIPInput = true;
            } else {
                m_hasBufferedSIPInput = false;
            }
            m_hasQueuedOppositeSIPInput = false;
            m_sipReadyAction = false;
            return false;
        }

        if (isCube && push && ticksFrame0 < targetHorizon && bestDelay == 0) {
            printf("[InputBuffer] CUBE PREDICTION: immediate jump may die within %d ticks; no safe timing found within %d ticks.\n",
                targetHorizon, maxDelayFrames);
        }

        m_gameHolding = push;
        return true;
    }

    void onPlayerUpdate(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || player != pl->m_player1) return;
        if (usesSIPTickClock()) return;
        if (GetAsyncKeyState(VK_F9) & 1) cyclePreset();
        advanceBufferTick(player);
    }

    void advanceBufferTick(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player || player != pl->m_player1) return;
        if (!m_enabled && m_waitTicks == -1) return;

        if (m_ticksSinceLastInput < 999) {
            m_ticksSinceLastInput++;
        }

        if (player->m_isShip || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip)) {
            flushPendingInput(pl);
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
            return;
        }

        if (m_waitTicks == 0) {
            m_SimFireButton(pl, m_bufferedPush);
            if (m_queuedOpposite) {
                m_waitTicks = -2;
                m_queuedOpposite = false;
            } else {
                clearPendingInput();
            }
        } else if (m_waitTicks > 0) {
            m_waitTicks--;
        } else if (m_waitTicks == -2) {
            m_SimFireButton(pl, !m_bufferedPush);
            clearPendingInput();
        } else if (m_waitTicks == -1) {
            // FAILSAFE: When no buffer is active, guarantee the game's hold state
            // matches your actual physical mouse button so Wave/Mini Wave can NEVER stay stuck holding!
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
        }
    }

    void m_SimFireButton(PlayLayer* pl, bool push) {
        m_firingBuffered = true;
        m_gameHolding = push;
        pl->handleButton(push, 1, true);
        if (pl->m_gameState.m_isDualMode && (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
            pl->handleButton(push, 1, false);
        }
        m_firingBuffered = false;
    }
};

static TrajectoryBufferSim s_sim;

$on_mod(Loaded) {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleTitleA("Geode Predictive Input Buffer - Live Console");
    printf("============================================================\n");
    printf(" [InputBuffer] v1.0.6 LOADED (Full audit: mode sync, trigger safety, ordered SIP subticks)\n");
    printf(" [InputBuffer] Starting Mode: STAGE 1 (2 All / 2 Wave)\n");
    printf(" [InputBuffer] Press F9 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/16 -> 24/24 -> OFF\n");
    printf("============================================================\n");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        s_sim.init();
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (s_sim.checkSimulationDeath(player)) return;
        PlayLayer::destroyPlayer(player, gameObject);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (s_sim.isSimulating()) {
            s_sim.markSimulationUnsupported(p0);
            return;
        }
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        PlayLayer::onQuit();
        s_sim.quit();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        s_sim.quit();
        return res;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
        if (s_sim.isSimulating()) {
            s_sim.markSimulationUnsupported();
            return;
        }
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (s_sim.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (s_sim.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (s_sim.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (!s_sim.isSimulating()) {
            s_sim.onPlayerUpdate(this);
        }
        PlayerObject::update(dt);
        if (PlayLayer::get() && !s_sim.isSimulating()) {
            s_sim.setFrameDelta(dt);
        }
    }

    void ringJump(RingObject* p0, bool p1) {
        if (s_sim.isSimulating()) {
            s_sim.markSimulationUnsupported(this);
            return;
        }
        PlayerObject::ringJump(p0, p1);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::processQueuedButtons", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::VeryEarly);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (s_sim.isSimulating()) {
            s_sim.markSimulationUnsupported(p0);
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        if (!s_sim.isSimulating() && s_sim.usesSIPTickClock()) {
            s_sim.filterSIPInputQueue(dt);
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!isHalfTick) s_sim.onSIPPhysicsTick();
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (s_sim.usesSIPTickClock()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        if (!s_sim.isSimulating() && !s_sim.isFiringBuffered() && button == 1 && PlayLayer::get()) {
            if (!s_sim.interceptInput(this, down, isPlayer1)) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (s_sim.isSimulating()) {
            gd::vector<GameObject*> extra;
            extra.reserve(objectsCount);
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                if (obj->m_objectType == GameObjectType::Solid ||
                    obj->m_objectType == GameObjectType::Hazard ||
                    obj->m_objectType == GameObjectType::AnimatedHazard ||
                    obj->m_objectType == GameObjectType::CollisionObject ||
                    obj->m_objectType == GameObjectType::Slope) {
                    extra.push_back(obj);
                } else if (typeinfo_cast<EffectGameObject*>(obj) || typeinfo_cast<RingObject*>(obj)) {
                    // This simulation deliberately cannot execute portals, pads, orbs and
                    // other trigger side effects. If one enters this candidate set, do not
                    // claim that the simplified path is safe.
                    s_sim.markSimulationUnsupported(player);
                }
            }
            GJBaseGameLayer::collisionCheckObjects(player, &extra, extra.size(), dt);
            return;
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_sim.isSimulating()) {
            s_sim.markSimulationUnsupported(player);
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};
