#ifndef _3DSSETTINGS_H_
#define _3DSSETTINGS_H_

// Menu tabs that can be marked for rebuilding. Kept in step with TAB_DIRTY_COUNT
// by a static assert next to that enum.
#define MENU_TAB_DIRTY_COUNT    5

// Largest per-slot depth the UI allows, in SNES pixels of parallax.
#define DEPTH3D_MAX     11

// Ceiling on the shift a slot can end up with once the 3D slider has scaled
// its depth. The slider alone cannot exceed DEPTH3D_MAX, so this only guards
// the rounding at the top of its travel.
#define DEPTH3D_SHIFT_MAX   DEPTH3D_MAX

// The SNES does not composite whole planes at one depth: it interleaves each
// plane's two tile priorities with the four sprite priorities, so a plane's
// high-priority tiles sit in front of sprites that are themselves in front of
// the same plane's low-priority tiles. Every one of those slots can therefore
// be given its own stereoscopic depth. Order here is the menu's order, not the
// hardware's -- which slot a plane's priority lands on depends on the
// background mode and is resolved while rendering.
typedef enum {
    DEPTH3D_BG1_PRIO0,
    DEPTH3D_BG1_PRIO1,
    DEPTH3D_BG2_PRIO0,
    DEPTH3D_BG2_PRIO1,
    DEPTH3D_BG3_PRIO0,
    DEPTH3D_BG3_PRIO1,
    DEPTH3D_BG4_PRIO0,
    DEPTH3D_BG4_PRIO1,
    DEPTH3D_OBJ_PRIO0,
    DEPTH3D_OBJ_PRIO1,
    DEPTH3D_OBJ_PRIO2,
    DEPTH3D_OBJ_PRIO3,

    // BG3's high-priority tiles have two places in the stack rather than one:
    // mode 1 can lift them from behind the sprites to in front of all of them,
    // with bit 3 of $2105. That is the largest move any plane makes, and the
    // two positions hold different things -- an overlay in front, scenery
    // behind -- so each gets its own depth. Appended so the slots before it
    // keep their numbering.
    DEPTH3D_BG3_PRIO1_FRONT,

    DEPTH3D_SLOT_COUNT,
} DEPTH3D_SLOT;

#define DEPTH3D_BG_SLOT(bg, priority)   ((bg) * 2 + (priority))
#define DEPTH3D_OBJ_SLOT(priority)      (DEPTH3D_OBJ_PRIO0 + (priority))

#include <stdio.h>
#include <array>
#include <limits.h>
#include <3ds.h>

// Where a slot sits in the frame the given background mode composites: the
// SNES depth it is drawn at, rear-most first, or -1 when the mode has no such
// plane. bg3Priority is the "BG3 highest priority" bit of $2105, which only
// means anything in mode 1; extbg is the EXTBG bit of $2133, which only means
// anything in mode 7.
int depth3dSlotDepth(int bgMode, bool bg3Priority, bool extbg, int slot);

// The two arrangements the hardware composites in, from appendix A-19: modes 0
// and 1 stack up to four backgrounds with the sprites between them, modes 2 to
// 7 stack two and interleave the sprites differently. A slot's place in the
// frame, and so the depth that suits it, belongs to one arrangement or the
// other, which is why each keeps its own set of depths.
typedef enum {
    DEPTH3D_FAMILY_MODE01,
    DEPTH3D_FAMILY_MODE27,
    DEPTH3D_FAMILY_COUNT,
} DEPTH3D_FAMILY;

#define DEPTH3D_FAMILY_OF(bgMode)   ((bgMode) <= 1 ? DEPTH3D_FAMILY_MODE01 : DEPTH3D_FAMILY_MODE27)

// The slots that arrangement composites, front-most first, and how many of them
// there are. A slot missing from a family has no place in its frames at all,
// rather than merely going unused by the mode in front of you.
const u8 *depth3dFamilySlots(int family, int *count);



#ifndef VERSION_MAJOR
#define VERSION_MAJOR 0
#endif

#ifndef VERSION_MINOR
#define VERSION_MINOR 0
#endif

#ifndef VERSION_MICRO
#define VERSION_MICRO 0
#endif

#define SCREEN_TOP_WIDTH        400
#define SCREEN_BOTTOM_WIDTH     320
#define SCREEN_HEIGHT           240

#define SAVESLOTS_MAX   5

#define HOTKEY_OPEN_MENU            0
#define HOTKEY_FAST_FORWARD_TOGGLE  1
#define HOTKEY_SWAP_CONTROLLERS     2
#define HOTKEY_SCREENSHOT           3
#define HOTKEY_QUICK_SAVE           4
#define HOTKEY_QUICK_LOAD           5
#define HOTKEY_SAVE_SLOT_NEXT       6
#define HOTKEY_SAVE_SLOT_PREV       7
#define HOTKEY_FAST_FORWARD_HOLD    8
#define HOTKEYS_COUNT   9

#define OPACITY_STEPS               20
#define SCANLINE_INTENSITY_MAX      8   // 47% brightness

#define MENU_ENTRY_CONTEXT_MENU     -2
#define MENU_CONTINUE_GAME          -3
#define MENU_SUBPAGE_ITEM_INFO      -4
#define MENU_ENTER_SUBPAGE          -100

namespace Setting {
    enum class ScreenFilter {
        Sharp,      // GPU_NEAREST
        Smooth,     // GPU_LINEAR
        Balanced,   // GPU_NEAREST base + low-alpha GPU_LINEAR overlay
    };

    enum class ScreenStretch {
        None,                  // 1:1 Native (256x224, 256x240)
        Aspect_4_3,            // Stretch width only to 298
        CrtAspect,             // Stretch width only to 292 (8:7 PAR)
        Fit_4_3,               // 4:3 Fit: Stretch to 320 x 240
        Fit_8_7,               // 8:7 Fit: Stretched when 224 lines, No Stretch when 240 lines (e.g. Super Mario Kart PAL)
        Full = 6,              // Fullscreen: Stretch to GameScreenWidth x 240
    };

    enum class ThumbnailMode {
        None,
        Boxart,
        Title,
        Gameplay,
    };

    enum class AssetMode {
        None,
        Default,       // Built-in
        Adaptive,      // Custom, else Default
        CustomOnly,    // Custom or nothing
    };

    enum class Theme {
        DarkMode,
        RetroArch,
        Original,
    };

    enum class Font {
        Tempesta,
        Ronda,
        Arial,
    };

    enum class Framerate {
        UseRomRegion,
        ForceFps60,
    };

    enum class Intensity3D {
        Standard,
        Medium,
        High,
    };


    enum class EnhancedResolution {
        Off,         // native 256px render
        Standard,    // 512px internal render (keeps 3D)
        Wide,        // 512px internal render + wide 800px screen (disables 3D)
    };
}

template <int Count>
struct ButtonMapping {
    std::array<u32, Count> MappingBitmasks;

    bool operator==(const ButtonMapping& other) const {
        return this->MappingBitmasks == other.MappingBitmasks;
    }

    bool IsHeld(u32 held3dsButtons) const {
        for (u32 mapping : MappingBitmasks) {
            if (mapping != 0 && (mapping & held3dsButtons) == mapping) {
                return true;
            }
        }

        return false;
    }

    void SetSingleMapping(u32 mapping) {
        SetDoubleMapping(mapping, 0);
    }

    void SetDoubleMapping(u32 mapping0, u32 mapping1) {
        if (Count > 0) {
            MappingBitmasks[0] = mapping0;
        }
        if (Count > 1) {
            MappingBitmasks[1] = mapping1;
        }
        if (Count > 2) {
            for (size_t i = 2; i < MappingBitmasks.size(); ++i) {
                MappingBitmasks[i] = 0;
            }
        }
    }
};

typedef struct {

    // --- GENERAL ---
    Setting::Theme Theme;
    Setting::Font Font;
    Setting::ThumbnailMode GameThumbnailType;
    gfxScreen_t GameScreen;
    bool Disable3DSlider;
    Setting::Intensity3D Intensity3D;
    bool LogFileEnabled;    // Write logs to sdmc:/3ds/snes9x_3ds/debug_<APP_VERSION>_session.log
    int CurrentSaveSlot;    // remember last used save slot (1 - 5)

    // --- FILE MENU ---
    char defaultDir[PATH_MAX];
    char lastSelectedDir[PATH_MAX];
    char lastSelectedFilename[NAME_MAX + 1];

    // --- RETROACHIEVEMENTS ---
    char RAUsername[32];
    char RAToken[64];

    // --- OSD & VIDEO ---
    Setting::AssetMode  GameOverlay;
    bool                GameOverlayAutoFit;
    int                 ScanlineIntensity;      // 0 - Off, 1..SCANLINE_INTENSITY_MAX - dark-row alpha
    Setting::AssetMode  GameScreenBg;
    int                 GameScreenBgOpacity;    // 20 - Maxium opacity
    Setting::AssetMode  SecondScreenBg;
    int                 SecondScreenBgOpacity;

    bool                ShowFPS;

    Setting::ScreenStretch ScreenStretch;
    Setting::ScreenFilter ScreenFilter;         // User preference for SNES_MAIN in stretched modes.
                                                // No Stretch enforces sharp (nearest) at render time.
    bool                CropEnabled;            // master toggle for the per-game crop/overscan settings
    int                 CropTop;                // top crop value in scanlines
    int                 CropBottom;             // bottom crop value in scanlines
    bool                Overscan;               // zoom (cropped) ingame screen to fit height

    // --- GAME-SPECIFIC ---
    int                 MaxFrameSkips;          // 0 - disable,
                                                // 1 - enable (max 1 consecutive skipped frame)
                                                // 2 - enable (max 2 consecutive skipped frames)
                                                // 3 - enable (max 3 consecutive skipped frames)
                                                // 4 - enable (max 4 consecutive skipped frames)


    Setting::Framerate  Framerate;              // 0 - Default based on Game region
                                                // 1 - Force 60 FPS

    int                 PaletteFix;             // Palette In-Frame Changes
                                                //   1 - Enabled - Default.
                                                //   2 - Disabled - Style 1.
                                                //   3 - Disabled - Style 2.

    u8                  PaletteDeferBgMask;     // Advanced: skip re-rendering these BG layers on mid-frame
                                                // palette changes (bit i = LAYER_BGi). 0 - render all.

    bool                Depth3DEnabled;         // Per-layer stereoscopic depth: give each SNES plane
                                                // its own depth instead of a flat game screen.

    bool                Depth3DCropEdges;       // Take the widest shift off all four screen edges,
                                                // so neither eye is left with a strip only it can see.

    bool                Depth3DFillGaps;        // Close the strip a background's two priorities
                                                // uncover between them when they sit at different
                                                // depths, by carrying the further-back one across it.

    bool                Depth3DMode7Perspective;// Let a Mode 7 plane recede across the screen,
                                                // taking each scanline's distance from the scale
                                                // Mode 7 is drawing it at, instead of standing the
                                                // whole plane at its slot's depth.

    s8                  Depth3D[DEPTH3D_FAMILY_COUNT][DEPTH3D_SLOT_COUNT];
                                                // Depth of each plane-and-priority slot, per
                                                // background-mode arrangement.
                                                // Unit is SNES pixels of parallax at a fully open 3D
                                                // slider: 0 = at the screen, > 0 = behind it,
                                                // < 0 = in front of it.

    bool                Mode7BilinearFilter;    // Bilinear filter for the Mode 7 background
                                                // texture. Default false; opt-in because it
                                                // changes the characteristic Mode 7 look.

    Setting::EnhancedResolution EnhancedResolution;  // Off / Standard (512px render) / 2x Screen (512px + wide)

    bool                RAEnabled;              // Enable RetroAchievements
    bool                RAChallengeIndicators;  // Game-screen badges for primed challenges
    bool                RAProgressIndicator;    // Game-screen badge + value for measured progress
    bool                RAEncoreMode;           // Re-attempt unlocked achievements

    int                 Volume;                 // 0: 100%, 1: 125%, 2: 150%, 3: 175%, 4: 200%
    int                 GlobalVolume;

    int                 AudioBuffer;            // wavebuf depth: 0=Low(4), 1=Normal(8), 2=High(16)

    bool                AutoSavestate;          // Automatically save the the current state when the emulator is closed or the game is changed
    bool                SaveStateScreenshots;   // Opt-in: save a screenshot next to each savestate

    int                 SRAMSaveInterval;       // SRAM Save Interval
                                                //   1 - 1 second.
                                                //   2 - 10 seconds
                                                //   3 - 60 seconds
                                                //   4 - Never

    bool                ForceSRAMWriteOnPause;  // If the SRAM should be written to SD even when no change was detected.
                                                // Some games (eg. Yoshi's Island) don't detect SRAM writes correctly.

    // --- CONTROLS ---
    std::array<::ButtonMapping<1>, HOTKEYS_COUNT> ButtonHotkeys;
    std::array<::ButtonMapping<1>, HOTKEYS_COUNT> GlobalButtonHotkeys;

    bool      BindCirclePad;                    // Use Circle Pad as D-Pad for gaming
    bool      GlobalBindCirclePad;

    std::array<std::array<int, 4>, 10> ButtonMapping;
    std::array<std::array<int, 4>, 10> GlobalButtonMapping;

    std::array<int, 8>   Turbo;                 // Turbo buttons: 0 - No turbo, 1 - Release/Press every alt frame.
                                                // Indexes: 0 - A, 1 - B, 2 - X, 3 - Y, 4 - L, 5 - R
    std::array<int, 8>   GlobalTurbo;

    bool      UseGlobalEmuControlKeys;          // Use global emulator control keys for all games
    bool      UseGlobalBindCirclePad;           // Use Circle Pad as D-Pad
    bool      UseGlobalButtonMappings;          // Use global button mappings for all games
    bool      UseGlobalTurbo;
    bool      UseGlobalVolume;

    // --- RUNTIME / CALCULATED ---
    // Not saved to config
    const char           *RootDir;

    gfxScreen_t         SecondScreen;
    int                 GameScreenWidth;
    int                 SecondScreenWidth;

    int                 StretchWidth;
    int                 StretchHeight;
    long                TicksPerFrame;

    bool                TurboMode;             // Effective fast-forward state (toggle and/or hold hotkeys)

    bool                LayerEnabled[8];       // Debug: per-layer enable toggle, index = LAYER_ID
                                               // (BG0-3, OBJ, Backdrop, Color Math, Brightness).
                                               // All true by default.

    bool                isNew3DS;
    bool                isRomFsLoaded;
    bool                isRomLoaded;
    bool                isDirty;               // needs saving to disk
    bool                cheatsDirty;
    bool                menuTabDirty[MENU_TAB_DIRTY_COUNT];
} S9xSettings3DS;

extern S9xSettings3DS settings3DS;

void settings3dsResetGlobalDefaults();
void settings3dsResetGameDefaults();
void settings3dsUpdate(bool includeGameSettings);
bool settingsSave(bool includeGameSettings);
void settings3dsApplyScreenLayout();
void settings3dsApplyScreenStretch();

const char *settings3dsGetAppVersion(const char *prefix, const char *suffix = NULL);

#endif
