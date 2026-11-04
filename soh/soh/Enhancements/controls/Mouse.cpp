#include <ship/Context.h>
#include <ship/window/Window.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "Mouse.h"
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <cmath>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/controller/controldevice/controller/mapping/mouse/MouseButtonToButtonMapping.h>
#include <fast/Fast3dWindow.h>
#include <ship/window/gui/Gui.h>

extern "C" {
#include "z64player.h"
#include "functions.h"
#include "macros.h"
}

static Ship::Coords mouseCoord = {};
static Ship::Coords mouseCoordRel = {};

// OOT recomp fork: mouse movement after acceleration and smoothing are applied.
// The camera and first-person aiming read these instead of the raw delta.
static f32 mouseRelX = 0.0f;
static f32 mouseRelY = 0.0f;

#define CVAR_ENABLE_MOUSE_NAME CVAR_SETTING("EnableMouse")
#define CVAR_ENABLE_MOUSE_DEFAULT 0
#define CVAR_ENABLE_MOUSE_VALUE CVarGetInteger(CVAR_ENABLE_MOUSE_NAME, CVAR_ENABLE_MOUSE_DEFAULT)
#define MOUSE_ENABLED (CVAR_ENABLE_MOUSE_VALUE && GetWindow()->IsMouseCaptured())

// OOT recomp fork settings
#define CVAR_MOUSE_CAM_SENS_X_NAME CVAR_SETTING("Mouse.CameraSensitivity.X")
#define CVAR_MOUSE_CAM_SENS_Y_NAME CVAR_SETTING("Mouse.CameraSensitivity.Y")
#define CVAR_MOUSE_SMOOTHING_NAME CVAR_SETTING("Mouse.Smoothing")
#define CVAR_MOUSE_ACCEL_NAME CVAR_SETTING("Mouse.Acceleration")

std::shared_ptr<Ship::Window> GetWindow() {
    return OTRGlobals::Instance->context->GetWindow();
}

extern "C" {
void Mouse_UpdatePos() {
    mouseCoord = GetWindow()->GetMousePos();
}

void Mouse_UpdatePosRel() {
    mouseCoordRel = GetWindow()->GetMouseDelta();

    f32 rawX = static_cast<f32>(mouseCoordRel.x);
    f32 rawY = static_cast<f32>(mouseCoordRel.y);

    if (!MOUSE_ENABLED) {
        mouseRelX = 0.0f;
        mouseRelY = 0.0f;
        return;
    }

    // Acceleration: faster mouse movement turns the camera proportionally further.
    f32 accel = CVarGetFloat(CVAR_MOUSE_ACCEL_NAME, 0.0f);
    if (accel > 0.0f) {
        f32 speed = sqrtf(rawX * rawX + rawY * rawY);
        f32 factor = 1.0f + accel * speed / 100.0f;
        if (factor > 4.0f) {
            factor = 4.0f;
        }
        rawX *= factor;
        rawY *= factor;
    }

    // Smoothing: blend with the previous frame's movement to remove jitter.
    f32 smoothing = CVarGetFloat(CVAR_MOUSE_SMOOTHING_NAME, 0.0f);
    if (smoothing < 0.0f) {
        smoothing = 0.0f;
    } else if (smoothing > 0.9f) {
        smoothing = 0.9f;
    }
    mouseRelX = mouseRelX * smoothing + rawX * (1.0f - smoothing);
    mouseRelY = mouseRelY * smoothing + rawY * (1.0f - smoothing);
}

void Mouse_UpdateAll() {
    Mouse_UpdatePos();
    Mouse_UpdatePosRel();
}

void Mouse_HandleThirdPerson(f32* newCamX, f32* newCamY) {
    if (MOUSE_ENABLED && !CVarGetInteger(CVAR_SETTING("DisableThirdPersonMouse"), 0)) {
        *newCamX -= mouseRelX * 40.0f * CVarGetFloat(CVAR_MOUSE_CAM_SENS_X_NAME, 1.0f);
        *newCamY -= mouseRelY * 40.0f * CVarGetFloat(CVAR_MOUSE_CAM_SENS_Y_NAME, 1.0f);
    }
}

void Mouse_HandleFirstPerson(Player* player) {
    f32 xAxisMulti = CVarGetFloat(CVAR_SETTING("FirstPersonCameraSensitivity.X"), 1.0f);
    f32 yAxisMulti = CVarGetFloat(CVAR_SETTING("FirstPersonCameraSensitivity.Y"), 1.0f);
    s8 invertXAxisMulti = ((CVarGetInteger(CVAR_SETTING("Controls.InvertAimingXAxis"), 0) &&
                            !CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) ||
                           (!CVarGetInteger(CVAR_SETTING("Controls.InvertAimingXAxis"), 0) &&
                            CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)))
                              ? -1
                              : 1;
    s8 invertYAxisMulti = CVarGetInteger(CVAR_SETTING("Controls.InvertAimingYAxis"), 1) ? -1 : 1;
    if (MOUSE_ENABLED) {
        player->actor.focus.rot.y -= static_cast<int16_t>(mouseRelX * 6.0f * xAxisMulti * invertXAxisMulti);
        player->actor.focus.rot.x += static_cast<int16_t>(mouseRelY * 6.0f * yAxisMulti * invertYAxisMulti);
    }
}

void Mouse_RecenterCursor() {
    u32 width = GetWindow()->GetWidth();
    u32 height = GetWindow()->GetHeight();
    if (MOUSE_ENABLED) {
        GetWindow()->SetMousePos({ static_cast<s32>(width / 2), static_cast<s32>(height / 2) });
    }
}

void Mouse_HandleShield(f32* sp50, f32* sp54) {
    if (MOUSE_ENABLED) {
        s32 width = GetWindow()->GetWidth();
        s32 height = GetWindow()->GetHeight();
        f32 xBound = 7200 / (width / 2.0f);
        f32 yBound = 6000 / (height / 2.0f);
        *sp50 +=
            (mouseCoord.x - (width / 2)) * xBound * (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? 1 : -1);
        *sp54 += (mouseCoord.y - (height / 2)) * yBound;
        *sp50 = CLAMP(*sp50, -7200, 7200);
        *sp54 = CLAMP(*sp54, -6000, 6000);
    }
}

static s8 iterMouse = 0;
static s32 mouseQuickspinX[5] = {};
static s32 mouseQuickspinY[5] = {};
static u8 quickspinCount = 0;

void Mouse_UpdateQuickspinCount() {
    if (MOUSE_ENABLED && !CVarGetInteger(CVAR_SETTING("DisableThirdPersonMouse"), 0)) {
        quickspinCount = (quickspinCount + 1) % 5;
        mouseQuickspinX[quickspinCount] = mouseCoord.x;
        mouseQuickspinY[quickspinCount] = mouseCoord.y;
    } else {
        quickspinCount = 0;
    }
}

bool Mouse_HandleQuickspin(bool* should, s8* iter2, s8* sp3C) {
    s8 temp1;
    s8 temp2;
    s32 i;
    if (!MOUSE_ENABLED || CVarGetInteger(CVAR_SETTING("DisableThirdPersonMouse"), 0)) {
        return *should = false;
    }

    for (i = 0; i < 4; i++, iter2++) {
        // Calculating angles as per z_lib.c:func_80077D10()
        s32 relY = mouseQuickspinY[i + 1] - mouseQuickspinY[i];
        s32 relX = mouseQuickspinX[i + 1] - mouseQuickspinX[i];
        s16 aTan = Math_Atan2S((f32)relY, (f32)-relX);
        iterMouse = (u16)(aTan + 0x2000) >> 9; // See z_player.c:Player_ProcessControlStick()
        if ((*iter2 = iterMouse) < 0) {
            return *should = false;
        }
        *iter2 *= 2;
    }
    temp1 = sp3C[0] - sp3C[1];
    if (ABS(temp1) < 10) {
        return *should = false;
    }
    iter2 = &sp3C[1];
    for (i = 1; i < 3; i++, iter2++) {
        temp2 = *iter2 - *(iter2 + 1);
        if ((ABS(temp2) < 10) || (temp2 * temp1 < 0)) {
            return *should = false;
        }
    }

    return *should = true;
}

// Hook handlers

void Mouse_RegisterRecenterCursorOnShield() {
    COND_HOOK(OnPlayerHoldUpShield, true, Mouse_RecenterCursor);
}

void Mouse_RegisterHandleFirstPerson() {
    COND_HOOK(OnPlayerFirstPersonControl, true, Mouse_HandleFirstPerson);
}

void Mouse_RegisterHandleShield() {
    COND_HOOK(OnPlayerShieldControl, true, Mouse_HandleShield);
}

void Mouse_RegisterUpdateQuickspinCount() {
    COND_HOOK(OnPlayerProcessStick, true, Mouse_UpdateQuickspinCount);
}

void Mouse_RegisterHandleQuickspin() {
    REGISTER_VB_SHOULD(VB_SHOULD_QUICKSPIN, { Mouse_HandleQuickspin(should, va_arg(args, s8*), va_arg(args, s8*)); });
}

static RegisterShipInitFunc registerShieldRecenter(Mouse_RegisterRecenterCursorOnShield, { CVAR_ENABLE_MOUSE_NAME });
static RegisterShipInitFunc registerFirstPerson(Mouse_RegisterHandleFirstPerson, { CVAR_ENABLE_MOUSE_NAME });
static RegisterShipInitFunc registerQuickspinCount(Mouse_RegisterUpdateQuickspinCount, { CVAR_ENABLE_MOUSE_NAME });
static RegisterShipInitFunc registerQuickspin(Mouse_RegisterHandleQuickspin, { CVAR_ENABLE_MOUSE_NAME });
static RegisterShipInitFunc registerShieldMove(Mouse_RegisterHandleShield, { CVAR_ENABLE_MOUSE_NAME });
} // extern "C"

// OOT recomp fork: one-click "Modern PC Controls" preset.
// Turns on mouse camera + aiming, binds mouse buttons and enables the options that make
// mouse and keyboard play well. Keyboard bindings are left as they are.
void Mouse_ApplyModernPcControls() {
    // Mouse camera and aiming
    CVarSetInteger(CVAR_ENABLE_MOUSE_NAME, 1);
    CVarSetInteger(CVAR_SETTING("AutoCaptureMouse"), 1);
    CVarSetInteger(CVAR_SETTING("DisableThirdPersonMouse"), 0);
    CVarSetInteger(CVAR_SETTING("FreeLook.Enabled"), 1);   // the mouse only moves the camera with Free Look on
    CVarSetInteger(CVAR_SETTING("A11yDisableIdleCam"), 1); // stop the camera swinging back behind Link
    CVarSetInteger(CVAR_SETTING("DisableFirstPersonAutoCenterView"), 1);

    // Aiming helpers
    CVarSetInteger(CVAR_ENHANCEMENT("BowReticle"), 1);
    CVarSetInteger(CVAR_ENHANCEMENT("BoomerangFirstPerson"), 1);
    CVarSetInteger(CVAR_ENHANCEMENT("BoomerangReticle"), 1);

    // Mouse buttons: left = sword (B), right = Z-target, middle = shield (R).
    // Existing mouse bindings are replaced; keyboard and controller bindings are kept.
    auto controller = Ship::Context::GetRawInstance()->GetControlDeck()->GetControllerByPort(0);
    if (controller != nullptr) {
        controller->ClearAllMappingsForDeviceType(Ship::PhysicalDeviceType::Mouse);
        const std::pair<CONTROLLERBUTTONS_T, Ship::MouseBtn> binds[] = {
            { BTN_B, Ship::LUS_MOUSE_BTN_LEFT },
            { BTN_Z, Ship::LUS_MOUSE_BTN_RIGHT },
            { BTN_R, Ship::LUS_MOUSE_BTN_MIDDLE },
        };
        for (const auto& [bitmask, mouseButton] : binds) {
            auto button = controller->GetButton(bitmask);
            if (button == nullptr) {
                continue;
            }
            auto mapping = std::make_shared<Ship::MouseButtonToButtonMapping>(0, bitmask, mouseButton);
            button->AddButtonMapping(mapping);
            mapping->SaveToConfig();
            button->SaveButtonMappingIdsToConfig();
        }
    }

    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(GetWindow());
    if (wnd != nullptr) {
        wnd->SetAutoCaptureMouse(true);
    }

    ShipInit::InitAll();
    GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}
