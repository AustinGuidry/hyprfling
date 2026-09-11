// hyprfling -- give floating windows momentum.
//
// Drag a floating window (SUPER + LMB) and let go while it's still moving: it
// keeps going, bleeds speed to friction, bounces off the edges of the work area
// and shoves other floating windows out of its way.
//
// Fling mode goes further: every window on screen comes loose from the layout
// and a plain click-drag throws it. Esc puts everything back.
//
// Lua API (Hyprland 0.55+ Lua config):
//   hl.plugin.fling.mode([on])       toggle fling mode, or set it; returns the new state
//   hl.plugin.fling.yeet([degrees])  throw the focused window; random if omitted
//   hl.plugin.fling.toggle()         turn physics on/off, returns the new state
//   hl.plugin.fling.stop()           freeze everything mid-flight

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

extern "C" {
#include <lua.h>
}

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

namespace {
    // Not `inline`: inline variables become STB_GNU_UNIQUE symbols, and a freshly
    // loaded copy of the plugin would silently share the stale copy's handle.
    HANDLE PHANDLE = nullptr;

    using namespace std::chrono_literals;

    // How far back release velocity is measured. Long enough to smooth out
    // jittery pointer events, short enough that a flick isn't averaged away.
    constexpr auto             SAMPLE_WINDOW = 80ms;
    // Holding a window still this long before letting go means "put it here".
    constexpr auto             STALE_RELEASE = 50ms;
    constexpr Time::steady_dur STEP          = 7ms;
    constexpr double           REST_SPEED    = 20.0; // px/s
    constexpr size_t           NO_BODY       = SIZE_MAX;

    const CHyprColor           ACCENT{0.4, 0.8, 1.0, 1.0};

    struct SSample {
        Time::steady_tp at;
        Vector2D        pos;
    };

    struct SBody {
        WP<Layout::ITarget>              target;
        Vector2D                         pos; // top-left, global coords
        Vector2D                         vel; // px/s
        bool                             grounded = false;
        // windows this one was already stacked on when it started moving
        std::vector<WP<Layout::ITarget>> ignore;
    };

    struct {
        SP<Config::Values::CFloatValue> friction, bounce, gravity, minSpeed, maxSpeed;
        SP<Config::Values::CBoolValue>  collide;
    } cfg;

    struct {
        bool                enabled = true;
        WP<Layout::ITarget> dragTarget;
        std::deque<SSample> samples;
        std::vector<SBody>  bodies;
        SP<CEventLoopTimer> timer;
        Time::steady_tp     lastStep;
        CHyprSignalListener mouseMove, mouseButton, key, workspace;
    } state;

    // A tiled window fling mode floated, to be tiled again on exit.
    struct SLoosened {
        WP<Layout::ITarget> target;
        // what the window floats at normally; floating it in place overwrites this
        Vector2D            floatingSize;
    };

    struct {
        bool                   active               = false;
        bool                   dragging             = false; // a drag started by fling mode, not a keybind
        bool                   swallowEscapeRelease = false;
        std::vector<SLoosened> loosened;
    } mode;

    enum eEdge : uint8_t {
        EDGE_NONE,
        EDGE_NEAR, // left / top
        EDGE_FAR,  // right / bottom
    };

    // Floating, visible, unpinned window sharing a workspace with `self`.
    SP<Layout::ITarget> neighbour(const PHLWINDOW& w, const PHLWINDOW& self) {
        if (!w || w == self || !w->m_isMapped || w->isHidden() || w->m_pinned || w->m_workspace != self->m_workspace)
            return {};

        const auto TARGET = w->layoutTarget();
        if (!TARGET || !TARGET->floating())
            return {};

        return TARGET;
    }

    Vector2D overlapOf(const CBox& a, const CBox& b) {
        return {std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x), std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y)};
    }

    std::vector<WP<Layout::ITarget>> overlapping(const SP<Layout::ITarget>& target) {
        std::vector<WP<Layout::ITarget>> out;
        const auto                       BOX = target->position();

        for (const auto& w : Desktop::windowState()->windows()) {
            const auto OTHER = neighbour(w, target->window());
            if (!OTHER)
                continue;

            const auto OVERLAP = overlapOf(BOX, OTHER->position());
            if (OVERLAP.x > 0 && OVERLAP.y > 0)
                out.emplace_back(OTHER);
        }

        return out;
    }

    size_t bodyIndex(const SP<Layout::ITarget>& target) {
        for (size_t i = 0; i < state.bodies.size(); ++i) {
            if (state.bodies[i].target.lock() == target)
                return i;
        }
        return NO_BODY;
    }

    bool ignores(const SBody& body, const SP<Layout::ITarget>& other) {
        return std::ranges::any_of(body.ignore, [&](const auto& w) { return w.lock() == other; });
    }

    void forget(SBody& body, const SP<Layout::ITarget>& other) {
        std::erase_if(body.ignore, [&](const auto& w) { return !w || w.lock() == other; });
    }

    eEdge bounceAxis(double& pos, double& vel, double size, double areaStart, double areaLen, double bounce) {
        const double MAXPOS = areaStart + std::max(0.0, areaLen - size);

        if (pos <= areaStart) {
            pos = areaStart;
            if (vel < 0)
                vel = -vel * bounce;
            return EDGE_NEAR;
        }

        if (pos >= MAXPOS) {
            pos = MAXPOS;
            if (vel > 0)
                vel = -vel * bounce;
            return EDGE_FAR;
        }

        return EDGE_NONE;
    }

    void wake() {
        if (state.timer->armed())
            return;

        state.lastStep = Time::steadyNow();
        state.timer->updateTimeout(STEP);
    }

    void launch(const SP<Layout::ITarget>& target, const Vector2D& vel) {
        std::erase_if(state.bodies, [&](const SBody& b) { return b.target.lock() == target; });
        state.bodies.push_back({.target = target, .pos = target->position().pos(), .vel = vel, .ignore = overlapping(target)});

        Log::logger->log(Log::DEBUG, "[hyprfling] launched {} at ({:.0f}, {:.0f}) px/s", target->window(), vel.x, vel.y);
        wake();
    }

    void collide(double bounce, const SP<Layout::ITarget>& dragged) {
        for (size_t i = 0; i < state.bodies.size(); ++i) {
            const auto A = state.bodies[i].target.lock();

            for (const auto& w : Desktop::windowState()->windows()) {
                const auto B = neighbour(w, A->window());
                if (!B || B == dragged)
                    continue;

                size_t j = bodyIndex(B);
                if (j != NO_BODY && j < i)
                    continue; // already resolved from B's side

                const CBox BOXA{state.bodies[i].pos, A->position().size()};
                const CBox BOXB     = j == NO_BODY ? B->position() : CBox{state.bodies[j].pos, B->position().size()};
                const auto OVERLAP  = overlapOf(BOXA, BOXB);
                const bool TOUCHING = OVERLAP.x > 0 && OVERLAP.y > 0;

                // Windows that were already stacked when one of them launched pass
                // through each other until they separate -- otherwise throwing a
                // window off a pile would blast the whole pile across the screen.
                if (ignores(state.bodies[i], B) || (j != NO_BODY && ignores(state.bodies[j], A))) {
                    if (!TOUCHING) {
                        forget(state.bodies[i], B);
                        if (j != NO_BODY)
                            forget(state.bodies[j], A);
                    }
                    continue;
                }

                if (!TOUCHING)
                    continue;

                if (j == NO_BODY) {
                    // A resting window got hit; it joins the simulation.
                    auto ignore = overlapping(B);
                    std::erase_if(ignore, [&](const auto& o) { return o.lock() == A; });
                    state.bodies.push_back({.target = B, .pos = BOXB.pos(), .vel = {}, .ignore = std::move(ignore)});
                    j = state.bodies.size() - 1;
                }

                auto& a = state.bodies[i];
                auto& b = state.bodies[j];

                // Resolve along the axis of least penetration. Mass is window area,
                // so a terminal bounces off a maximized browser instead of moving it.
                const bool   ALONG_X = OVERLAP.x < OVERLAP.y;
                const double INV_A   = 1.0 / std::max(1.0, BOXA.w * BOXA.h);
                const double INV_B   = 1.0 / std::max(1.0, BOXB.w * BOXB.h);
                const double NORMAL  = (ALONG_X ? BOXA.middle().x < BOXB.middle().x : BOXA.middle().y < BOXB.middle().y) ? 1.0 : -1.0;
                const double PEN     = ALONG_X ? OVERLAP.x : OVERLAP.y;

                double&      posA = ALONG_X ? a.pos.x : a.pos.y;
                double&      posB = ALONG_X ? b.pos.x : b.pos.y;
                double&      velA = ALONG_X ? a.vel.x : a.vel.y;
                double&      velB = ALONG_X ? b.vel.x : b.vel.y;

                posA -= NORMAL * PEN * INV_A / (INV_A + INV_B);
                posB += NORMAL * PEN * INV_B / (INV_A + INV_B);

                const double APPROACH = (velB - velA) * NORMAL;
                if (APPROACH < 0) {
                    const double IMPULSE = -(1.0 + bounce) * APPROACH / (INV_A + INV_B);
                    velA -= IMPULSE * INV_A * NORMAL;
                    velB += IMPULSE * INV_B * NORMAL;
                }
            }
        }
    }

    void step() {
        const auto NOW = Time::steadyNow();
        const auto DT  = std::clamp(std::chrono::duration<double>(NOW - state.lastStep).count(), 0.0, 1.0 / 30.0);
        state.lastStep = NOW;

        const auto& DRAG    = g_layoutManager->dragController();
        const auto  DRAGGED = DRAG->mode() == MBIND_MOVE ? DRAG->target() : SP<Layout::ITarget>{};

        // Grabbing a window mid-flight catches it.
        std::erase_if(state.bodies, [&](const SBody& b) {
            const auto T = b.target.lock();
            return !T || T == DRAGGED || !T->floating() || !T->space() || !T->window() || !T->window()->m_isMapped || T->window()->isHidden();
        });

        const double BOUNCE  = std::clamp<double>(cfg.bounce->value(), 0.0, 1.0);
        const double GRAVITY = cfg.gravity->value();
        const double DECAY   = std::exp(-cfg.friction->value() * DT);

        for (auto& b : state.bodies) {
            const auto T    = b.target.lock();
            const auto SIZE = T->position().size();
            const auto AREA = T->space()->workArea(true);

            b.vel.y += GRAVITY * DT;
            b.vel = b.vel * DECAY;
            b.pos = b.pos + b.vel * DT;

            bounceAxis(b.pos.x, b.vel.x, SIZE.x, AREA.x, AREA.w, BOUNCE);
            const auto EDGE = bounceAxis(b.pos.y, b.vel.y, SIZE.y, AREA.y, AREA.h, BOUNCE);
            b.grounded      = GRAVITY > 0 ? EDGE == EDGE_FAR : GRAVITY < 0 ? EDGE == EDGE_NEAR : true;
        }

        if (cfg.collide->value())
            collide(BOUNCE, DRAGGED);

        for (auto& b : state.bodies) {
            const auto T = b.target.lock();
            const auto W = T->window();

            g_pHyprRenderer->damageWindow(W);
            g_layoutManager->setTargetGeom(CBox{b.pos, T->position().size()}, T);
            T->warpPositionSize();
            g_pHyprRenderer->damageWindow(W);
        }

        // Out of energy and nothing pulling on it: go to sleep. With gravity on,
        // a window only rests once it's lying on the floor, not at the top of an arc.
        std::erase_if(state.bodies, [](const SBody& b) { return b.grounded && std::hypot(b.vel.x, b.vel.y) < REST_SPEED; });

        if (!state.bodies.empty())
            state.timer->updateTimeout(STEP);
    }

    // Float every tiled window `pick` accepts, leaving it exactly where it is on
    // screen, and remember it so fling mode can tile it again on the way out.
    void loosen(const std::function<bool(const PHLWINDOW&)>& pick) {
        struct SPending {
            SP<Layout::ITarget> target;
            CBox                box;
        };

        // Measure everything before floating anything: floating one window makes
        // the layout stretch its neighbours into the space it left. The window's
        // own box, not the target's -- a tiled target's box is its whole slot,
        // gaps included.
        std::vector<SPending> pending;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isMapped || w->isHidden() || !w->m_workspace || !pick(w) || Fullscreen::controller()->isFullscreen(w))
                continue;

            const auto TARGET = w->layoutTarget();
            if (TARGET && !TARGET->floating())
                pending.push_back({TARGET, CBox{w->position(Desktop::View::IGeometric::GEOMETRIC_GOAL), w->size(Desktop::View::IGeometric::GEOMETRIC_GOAL)}});
        }

        for (const auto& [target, box] : pending) {
            const auto FLOATING_SIZE = target->lastFloatingSize();

            g_layoutManager->changeFloatingMode(target);
            if (!target->floating())
                continue;

            g_layoutManager->setTargetGeom(box, target);
            target->warpPositionSize();
            mode.loosened.push_back({.target = target, .floatingSize = FLOATING_SIZE});
        }
    }

    void setMode(bool on) {
        if (on == mode.active)
            return;

        if (on) {
            mode.active   = true;
            state.enabled = true;
            loosen([](const PHLWINDOW& w) { return w->m_workspace->isVisible(); });
            HyprlandAPI::addNotification(PHANDLE, "[hyprfling] fling mode -- drag anything, Esc to stop", ACCENT, 3000);
            return;
        }

        if (mode.dragging) {
            CKeybindManager::changeMouseBindMode(MBIND_INVALID);
            mode.dragging = false;
        }

        mode.active = false;
        state.bodies.clear();
        state.samples.clear();
        state.dragTarget.reset();

        for (const auto& [target, floatingSize] : mode.loosened) {
            const auto TARGET = target.lock();
            if (!TARGET || !TARGET->floating() || !TARGET->window() || !TARGET->window()->m_isMapped)
                continue;

            g_layoutManager->changeFloatingMode(TARGET);
            // tiling it records its fling-mode size as the floating size; put back the real one
            if (floatingSize.x > 0 && floatingSize.y > 0)
                TARGET->rememberFloatingSize(floatingSize);
        }
        mode.loosened.clear();

        HyprlandAPI::addNotification(PHANDLE, "[hyprfling] fling mode off", ACCENT, 1500);
    }

    void onMouseMove() {
        const auto& DRAG   = g_layoutManager->dragController();
        const auto  TARGET = DRAG->target();

        if (DRAG->mode() != MBIND_MOVE || !TARGET || DRAG->draggingTiled())
            return;

        if (state.dragTarget.lock() != TARGET) {
            state.samples.clear();
            state.dragTarget = TARGET;
        }

        const auto NOW = Time::steadyNow();
        state.samples.push_back({NOW, g_pInputManager->getMouseCoordsInternal()});
        while (NOW - state.samples.front().at > SAMPLE_WINDOW)
            state.samples.pop_front();
    }

    // The end of a drag: throw the window if it was still moving.
    void release() {
        const auto TARGET  = state.dragTarget.lock();
        const auto SAMPLES = state.samples;
        state.dragTarget.reset();
        state.samples.clear();

        if (!state.enabled || !TARGET || !TARGET->floating() || SAMPLES.size() < 2)
            return;

        if (Time::steadyNow() - SAMPLES.back().at > STALE_RELEASE)
            return;

        const double DT = std::chrono::duration<double>(SAMPLES.back().at - SAMPLES.front().at).count();
        if (DT < 0.005)
            return;

        auto         vel   = (SAMPLES.back().pos - SAMPLES.front().pos) / DT;
        const double SPEED = std::hypot(vel.x, vel.y);

        Log::logger->log(Log::DEBUG, "[hyprfling] release: {} samples over {:.3f}s, {:.0f} px/s", SAMPLES.size(), DT, SPEED);

        if (SPEED < cfg.minSpeed->value())
            return;

        if (SPEED > cfg.maxSpeed->value())
            vel = vel * (cfg.maxSpeed->value() / SPEED);

        launch(TARGET, vel);
    }

    // In fling mode a plain left click grabs the floating window under the
    // cursor and the app never sees it. Returns true if the event was used.
    bool modeButton(const IPointer::SButtonEvent& e) {
        if (!mode.active || e.button != BTN_LEFT)
            return false;

        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            const auto WINDOW = Desktop::viewState()->hitTest().windowAt(g_pInputManager->getMouseCoordsInternal(),
                                                                         Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);
            const auto TARGET = WINDOW ? WINDOW->layoutTarget() : SP<Layout::ITarget>{};
            if (!TARGET || !TARGET->floating())
                return false; // bars, empty desktop: the click goes through

            CKeybindManager::changeMouseBindMode(MBIND_MOVE);
            mode.dragging = true;
            return true;
        }

        if (!mode.dragging)
            return false;

        release();
        CKeybindManager::changeMouseBindMode(MBIND_INVALID);
        mode.dragging = false;
        return true;
    }

    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        if (modeButton(e)) {
            info.cancelled = true;
            return;
        }

        if (e.state == WL_POINTER_BUTTON_STATE_RELEASED)
            release();
    }

    void onKey(const IKeyboard::SKeyEvent& e, Event::SCallbackInfo& info) {
        if (e.keycode != KEY_ESC)
            return;

        if (e.state == WL_KEYBOARD_KEY_STATE_PRESSED && mode.active) {
            info.cancelled             = true;
            mode.swallowEscapeRelease = true;
            setMode(false);
        } else if (e.state == WL_KEYBOARD_KEY_STATE_RELEASED && mode.swallowEscapeRelease) {
            // the app never saw the press, so it doesn't get the release either
            info.cancelled             = true;
            mode.swallowEscapeRelease = false;
        }
    }

    int luaMode(lua_State* L) {
        setMode(lua_isboolean(L, 1) ? lua_toboolean(L, 1) : !mode.active);
        lua_pushboolean(L, mode.active);
        return 1;
    }

    int luaToggle(lua_State* L) {
        state.enabled = !state.enabled;
        if (!state.enabled) {
            setMode(false);
            state.bodies.clear();
        }

        HyprlandAPI::addNotification(PHANDLE, state.enabled ? "[hyprfling] physics on" : "[hyprfling] physics off", ACCENT, 1500);
        lua_pushboolean(L, state.enabled);
        return 1;
    }

    int luaStop(lua_State* L) {
        state.bodies.clear();
        return 0;
    }

    // Returns true, or false plus the reason nothing was thrown.
    int luaYeet(lua_State* L) {
        const auto fail = [L](const char* why) {
            lua_pushboolean(L, false);
            lua_pushstring(L, why);
            return 2;
        };

        const auto WINDOW = Desktop::focusState()->window();
        const auto TARGET = WINDOW ? WINDOW->layoutTarget() : SP<Layout::ITarget>{};
        if (!state.enabled)
            return fail("physics is off");
        if (!TARGET || !TARGET->floating())
            return fail("focused window isn't floating");

        double angle = 0;
        if (lua_isnoneornil(L, 1)) {
            static std::mt19937 rng{std::random_device{}()};
            angle = std::uniform_real_distribution<double>{0.0, 2.0 * std::numbers::pi}(rng);
        } else if (lua_isnumber(L, 1))
            angle = lua_tonumber(L, 1) * std::numbers::pi / 180.0;
        else
            return fail("expected an angle in degrees");

        const double SPEED = cfg.maxSpeed->value() * 0.6;
        launch(TARGET, {std::cos(angle) * SPEED, std::sin(angle) * SPEED});

        lua_pushboolean(L, true);
        return 1;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprfling] built against different Hyprland headers than the running compositor -- rebuild it",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hyprfling] version mismatch");
    }

    using namespace Config::Values;
    cfg.friction = makeShared<CFloatValue>("plugin:fling:friction", "how fast flung windows lose speed (1/s)", 1.6F, SFloatValueOptions{.min = 0.F});
    cfg.bounce   = makeShared<CFloatValue>("plugin:fling:bounce", "fraction of speed kept when hitting an edge or window", 0.7F, SFloatValueOptions{.min = 0.F, .max = 1.F});
    cfg.gravity  = makeShared<CFloatValue>("plugin:fling:gravity", "downward pull on flung windows (px/s^2), 0 to disable", 0.F);
    cfg.minSpeed = makeShared<CFloatValue>("plugin:fling:min_speed", "release speed needed to fling instead of drop (px/s)", 400.F, SFloatValueOptions{.min = 0.F});
    cfg.maxSpeed = makeShared<CFloatValue>("plugin:fling:max_speed", "speed limit (px/s)", 6000.F, SFloatValueOptions{.min = 1.F});
    cfg.collide  = makeShared<CBoolValue>("plugin:fling:collide", "flung windows knock other floating windows around", true);

    for (const SP<IValue>& v : {SP<IValue>(cfg.friction), SP<IValue>(cfg.bounce), SP<IValue>(cfg.gravity), SP<IValue>(cfg.minSpeed), SP<IValue>(cfg.maxSpeed),
                                SP<IValue>(cfg.collide)}) {
        HyprlandAPI::addConfigValueV2(PHANDLE, v);
    }

    // Reload before registering Lua functions: the reload is what picks up the
    // new config values, and it rebuilds the Lua state.
    HyprlandAPI::reloadConfig();

    for (const auto& [name, fn] : {std::pair{"mode", &luaMode}, std::pair{"yeet", &luaYeet}, std::pair{"toggle", &luaToggle}, std::pair{"stop", &luaStop}}) {
        const bool OK = HyprlandAPI::addLuaFunction(PHANDLE, "fling", name, fn);
        Log::logger->log(OK ? Log::DEBUG : Log::WARN, "[hyprfling] register hl.plugin.fling.{}: {}", name, OK ? "ok" : "FAILED");
        if (!OK)
            HyprlandAPI::addNotification(PHANDLE, std::format("[hyprfling] couldn't register hl.plugin.fling.{}", name), CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
    }

    state.mouseMove   = Event::bus()->m_events.input.mouse.move.listen([](auto&&, auto&&) { onMouseMove(); });
    state.mouseButton = Event::bus()->m_events.input.mouse.button.listen([](auto&& e, auto&& info) { onMouseButton(e, info); });
    state.key         = Event::bus()->m_events.input.keyboard.key.listen([](auto&& e, auto&& info) { onKey(e, info); });

    // Switching workspaces mid-mode shouldn't leave you facing windows you can't throw.
    state.workspace = Event::bus()->m_events.workspace.active.listen([](auto&& ws) {
        if (mode.active)
            loosen([&ws](const PHLWINDOW& w) { return w->m_workspace == ws; });
    });

    state.timer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { step(); }, nullptr);
    g_pEventLoopManager->addTimer(state.timer);

    HyprlandAPI::addNotification(PHANDLE, "[hyprfling] loaded -- drag a floating window and let go", ACCENT, 4000);

    return {"hyprfling", "Throw floating windows around", "AustinGuidry", "0.2"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    setMode(false);

    if (state.timer) {
        g_pEventLoopManager->removeTimer(state.timer);
        state.timer.reset();
    }

    state.mouseMove.reset();
    state.mouseButton.reset();
    state.key.reset();
    state.workspace.reset();
    state.bodies.clear();
    state.samples.clear();
}
