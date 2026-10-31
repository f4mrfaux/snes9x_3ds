
#include <cstring>

#include "snes9x.h"
#include "memmap.h"
#include "3dssettings.h"
#include "3dssound.h"
#include "3dslcd.h"
#include "3dsui.h"

S9xSettings3DS settings3DS;

void settings3dsResetGlobalDefaults() {
    settings3DS.RootDir = "sdmc:/3ds/snes9x_3ds";
    
    memset(settings3DS.defaultDir, 0, sizeof(settings3DS.defaultDir));
    memset(settings3DS.lastSelectedDir, 0, sizeof(settings3DS.lastSelectedDir));
    memset(settings3DS.lastSelectedFilename, 0, sizeof(settings3DS.lastSelectedFilename));
    
    settings3DS.Theme = Setting::Theme::DarkMode;
    settings3DS.Font  = Setting::Font::Tempesta;
    settings3DS.GameThumbnailType = Setting::ThumbnailMode::None;
    settings3DS.SaveStateScreenshots = false;
    settings3DS.GameScreen = GFX_TOP;
    
    settings3DS.Disable3DSlider = false;
    settings3DS.Intensity3D = Setting::Intensity3D::Standard;
    settings3DS.LogFileEnabled = false;

    settings3DS.ScreenStretch = Setting::ScreenStretch::Aspect_4_3;
    settings3DS.ScreenFilter = Setting::ScreenFilter::Smooth;
    settings3dsApplyScreenStretch();
    
    settings3DS.TicksPerFrame = TICKS_PER_FRAME_SNES_NTSC;
    settings3DS.GlobalVolume = 2;

    settings3DS.GameOverlay = Setting::AssetMode::None;
    settings3DS.GameOverlayAutoFit = false;
    settings3DS.ScanlineIntensity = 0;
    settings3DS.GameScreenBg = Setting::AssetMode::Adaptive;
    settings3DS.GameScreenBgOpacity = OPACITY_STEPS / 2;
    settings3DS.SecondScreenBg = Setting::AssetMode::Adaptive;
    settings3DS.SecondScreenBgOpacity = OPACITY_STEPS / 2;

    settings3DS.ShowFPS = false;

    settings3DS.UseGlobalEmuControlKeys = true;
    settings3DS.UseGlobalBindCirclePad = true;
    settings3DS.UseGlobalButtonMappings = true;
    settings3DS.UseGlobalTurbo = false;
    settings3DS.UseGlobalVolume = false;

    u32 defaultButtonMapping[] = { 
      SNES_A_MASK, SNES_B_MASK, SNES_X_MASK, SNES_Y_MASK, SNES_TL_MASK, SNES_TR_MASK, 0, 0, SNES_SELECT_MASK, SNES_START_MASK 
    };

    for (int i = 0; i < 10; i++)
      settings3DS.GlobalButtonMapping[i][0] = defaultButtonMapping[i];

    settings3DS.GlobalBindCirclePad = true;

    for (int i = 0; i < HOTKEYS_COUNT; ++i)
      settings3DS.ButtonHotkeys[i].SetSingleMapping(0);

    for (int i = 0; i < 8; i++)
      settings3DS.GlobalTurbo[i] = 0;

    settings3DS.isDirty = true;
}

void settings3dsResetGameDefaults() {
    settings3DS.Framerate = Setting::Framerate::UseRomRegion;
    settings3DS.PaletteFix = 0;
    settings3DS.PaletteDeferBgMask = 0;
    settings3DS.Mode7BilinearFilter = false;

    settings3DS.Depth3DEnabled = false;
    settings3DS.Depth3DCropEdges = true;
    settings3DS.Depth3DFillGaps = true;
    settings3DS.Depth3DMode7Perspective = true;
    memset(settings3DS.Depth3D, 0, sizeof(settings3DS.Depth3D));

    memset(settings3DS.LayerEnabled, true, sizeof(settings3DS.LayerEnabled));

    settings3DS.EnhancedResolution = Setting::EnhancedResolution::Off;
    settings3DS.RAEnabled = true;
    settings3DS.RAChallengeIndicators = true;
    settings3DS.RAProgressIndicator = true;
    settings3DS.RAEncoreMode = false;
    settings3DS.CropEnabled = false;
    settings3DS.CropTop = 0;
    settings3DS.CropBottom = 0;
    settings3DS.Overscan = false;
    settings3DS.Volume = settings3DS.GlobalVolume;
    settings3DS.MaxFrameSkips = 1;
    settings3DS.CurrentSaveSlot = 1;
    settings3DS.AutoSavestate = false;
    settings3DS.SRAMSaveInterval = 4;   // Disabled
    settings3DS.ForceSRAMWriteOnPause = false;
    settings3DS.AudioBuffer = 1;

    // reset controls to global defaults (settings.cfg)
    //
    settings3DS.BindCirclePad = settings3DS.GlobalBindCirclePad;

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 4; j++) {
            settings3DS.ButtonMapping[i][j] = settings3DS.GlobalButtonMapping[i][j];
        }
    }
    
    for (int i = 0; i < 8; i++) {
        settings3DS.Turbo[i] = settings3DS.GlobalTurbo[i];
    }

    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        settings3DS.ButtonHotkeys[i] = settings3DS.GlobalButtonHotkeys[i];
    }
}

void settings3dsApplyScreenStretch() {
    settings3DS.StretchWidth = 256;
    settings3DS.StretchHeight = -1;

    switch (settings3DS.ScreenStretch)
    {
        case Setting::ScreenStretch::None:
            break;

        case Setting::ScreenStretch::Aspect_4_3:
            settings3DS.StretchWidth = 298;
            break;

        case Setting::ScreenStretch::CrtAspect:
            settings3DS.StretchWidth = 292;
            break;

        case Setting::ScreenStretch::Fit_4_3:
            settings3DS.StretchWidth = 320;
            settings3DS.StretchHeight = SCREEN_HEIGHT;
            break;

        case Setting::ScreenStretch::Full:
            settings3DS.StretchWidth = settings3DS.GameScreen == GFX_TOP ? SCREEN_TOP_WIDTH : SCREEN_BOTTOM_WIDTH;
            settings3DS.StretchHeight = SCREEN_HEIGHT;
            break;

        case Setting::ScreenStretch::Fit_8_7:
            settings3DS.StretchWidth = 274;
            settings3DS.StretchHeight = SCREEN_HEIGHT;
            break;
    }
}


// Per-game default for the In-Frame Palette Changes.
// 1 = Enabled, 2 = Disabled Style 1, 3 = Disabled Style 2
static int settings3dsGetGameDefaultPaletteFix()
{
    const char *name = Memory.ROMName;

    if (settings3DS.isNew3DS) 
        return 1;

    if (strcmp(name, "Bahamut Lagoon") == 0 ||
        strcmp(name, "Bahamut Lagoon Eng v3") == 0 ||
        strcmp(name, "GUN HAZARD") == 0)
        return 2;   // dialog / flashing sky palette colours

    if (strncmp(name, "JUDGE DREDD THE MOVIE", 11) == 0 ||
        strcmp(name, "Secret of MANA") == 0 ||
        strcmp(name, "SeikenDensetsu 2") == 0 ||
        strcmp(name, "WILD GUNS") == 0 ||
        strcmp(name, "BATMAN FOREVER") == 0 ||
        strcmp(name, "KIRBY SUPER DELUXE") == 0)
        return 1;

    return 3;
}

void settings3dsUpdate(bool includeGameSettings)
{
    settings3dsApplyScreenStretch();

    if (includeGameSettings)
    {
        // Update frame rate
        //
        if (Settings.PAL) {
            settings3DS.TicksPerFrame = settings3DS.Framerate == Setting::Framerate::ForceFps60 ? TICKS_PER_FRAME_SNES_NTSC : TICKS_PER_FRAME_SNES_PAL;
        } else {
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_SNES_NTSC;
        }
        
        snd3dsApplyOutputVolume();

        if (settings3DS.PaletteFix == 0)
            settings3DS.PaletteFix = settings3dsGetGameDefaultPaletteFix();

        if (settings3DS.PaletteFix == 1)
            SNESGameFixes.PaletteCommitLine = -2;
        else if (settings3DS.PaletteFix == 2)
            SNESGameFixes.PaletteCommitLine = 1;
        else // 3
            SNESGameFixes.PaletteCommitLine = -1;

        if (settings3DS.SRAMSaveInterval == 1)
            Settings.AutoSaveDelay = 60;
        else if (settings3DS.SRAMSaveInterval == 2)
            Settings.AutoSaveDelay = 600;
        else if (settings3DS.SRAMSaveInterval == 3)
            Settings.AutoSaveDelay = 3600;
        else
            Settings.AutoSaveDelay = -1;

        if (settings3DS.UseGlobalButtonMappings) {
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 4; j++)
                    settings3DS.ButtonMapping[i][j] = settings3DS.GlobalButtonMapping[i][j];
            
            settings3DS.BindCirclePad = settings3DS.GlobalBindCirclePad;
        }

        if (settings3DS.UseGlobalTurbo) {
            for (int i = 0; i < 8; i++) 
                settings3DS.Turbo[i] = settings3DS.GlobalTurbo[i];
        }

        if (settings3DS.UseGlobalEmuControlKeys) {
             for (int i = 0; i < HOTKEYS_COUNT; ++i) 
                settings3DS.ButtonHotkeys[i] = settings3DS.GlobalButtonHotkeys[i];
        }
        
        // Fixes the Auto-Save timer bug that causes
        // the SRAM to be saved once when the settings were
        // changed to Disabled.
        //
        if (Settings.AutoSaveDelay == -1)
            CPU.AutoSaveTimer = -1;
        else
            CPU.AutoSaveTimer = 0;
    }
}

const char *settings3dsGetAppVersion(const char *prefix, const char *suffix) {
    static char version[64];

    if (VERSION_MICRO > 0) {
        snprintf(version, sizeof(version), "%s%d.%d.%d%s", prefix, VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, suffix != NULL ? suffix : "");
    } else {
        snprintf(version, sizeof(version), "%s%d.%d%s", prefix, VERSION_MAJOR, VERSION_MINOR, suffix != NULL ? suffix : "");
    }

    return version;
}

//---------------------------------------------------------
// Which SNES depth each plane-and-priority is composited at,
// per background mode. This mirrors the depths the renderer
// hands to the draw macros in S9xUpdateScreenHardware
// (gfxhw.cpp), which are in turn the order the development
// manual draws in appendix A-19: rear to front, the four
// sprite priorities interleaved between the planes at
// 3, 6, 9 and 12.
//
// -1 marks a plane the mode does not have.
//---------------------------------------------------------
static const s8 depth3dBgDepths[8][4][2] = {
    // BG1        BG2        BG3        BG4
    { {  8, 11 }, {  7, 10 }, {  2,  5 }, {  1,  4 } },   // mode 0
    { {  8, 11 }, {  7, 10 }, {  2,  5 }, { -1, -1 } },   // mode 1, BG3 prio 1 -> 13 with $2105 D3
    { {  5, 11 }, {  2,  8 }, { -1, -1 }, { -1, -1 } },   // mode 2
    { {  5, 11 }, {  2,  8 }, { -1, -1 }, { -1, -1 } },   // mode 3
    { {  5, 11 }, {  2,  8 }, { -1, -1 }, { -1, -1 } },   // mode 4
    { {  5, 11 }, {  2,  8 }, { -1, -1 }, { -1, -1 } },   // mode 5
    { {  5, 11 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },   // mode 6
    // Mode 7's plane is BG1 with one priority. BG2's two are the EXTBG plane,
    // which is one plane drawn twice: its pixels carry a priority bit of their
    // own and each pass keeps the pixels belonging to it.
    { {  5, -1 }, {  2,  8 }, { -1, -1 }, { -1, -1 } },   // mode 7
};

int depth3dSlotDepth(int bgMode, bool bg3Priority, bool extbg, int slot) {
    if (slot < 0 || slot >= DEPTH3D_SLOT_COUNT)
        return -1;

    if (bgMode < 0 || bgMode > 7)
        return -1;

    // Without EXTBG there is no second Mode 7 plane, so neither of BG2's rows
    // is in the frame.
    if (bgMode == 7 && !extbg && slot >= DEPTH3D_BG2_PRIO0 && slot <= DEPTH3D_BG2_PRIO1)
        return -1;

    // The one bit a game can move a plane with. Only one of BG3's two
    // high-priority slots is in the frame at a time, and $2105 bit 3 says
    // which. Snes9x reads that bit in mode 1 alone, which is what appendix
    // A-19 shows, though the register page says mode 0 or 1.
    bool lifted = bgMode == 1 && bg3Priority;

    if (slot == DEPTH3D_BG3_PRIO1_FRONT)
        return lifted ? 13 : -1;

    if (slot == DEPTH3D_BG3_PRIO1 && lifted)
        return -1;

    if (slot >= DEPTH3D_OBJ_PRIO0)
        return (slot - DEPTH3D_OBJ_PRIO0 + 1) * 3;

    return depth3dBgDepths[bgMode][slot / 2][slot & 1];
}

//---------------------------------------------------------
// The slots each arrangement composites, front-most first.
//
// Within one arrangement the order is the hardware's and does not change, so a
// row stays where it is while a game plays. Between the two it does change --
// the sprites fall in different places among the backgrounds -- which is why
// they are separate lists rather than one list that greys down.
//---------------------------------------------------------
static const u8 depth3dStackMode01[] = {
    DEPTH3D_BG3_PRIO1_FRONT,    // 13, only while $2105 bit 3 is set in mode 1
    DEPTH3D_OBJ_PRIO3,          // 12
    DEPTH3D_BG1_PRIO1,          // 11
    DEPTH3D_BG2_PRIO1,          // 10
    DEPTH3D_OBJ_PRIO2,          //  9
    DEPTH3D_BG1_PRIO0,          //  8
    DEPTH3D_BG2_PRIO0,          //  7
    DEPTH3D_OBJ_PRIO1,          //  6
    DEPTH3D_BG3_PRIO1,          //  5
    DEPTH3D_BG4_PRIO1,          //  4
    DEPTH3D_OBJ_PRIO0,          //  3
    DEPTH3D_BG3_PRIO0,          //  2
    DEPTH3D_BG4_PRIO0,          //  1
};

static const u8 depth3dStackMode27[] = {
    DEPTH3D_OBJ_PRIO3,          // 12
    DEPTH3D_BG1_PRIO1,          // 11
    DEPTH3D_OBJ_PRIO2,          //  9
    DEPTH3D_BG2_PRIO1,          //  8
    DEPTH3D_OBJ_PRIO1,          //  6
    DEPTH3D_BG1_PRIO0,          //  5
    DEPTH3D_OBJ_PRIO0,          //  3
    DEPTH3D_BG2_PRIO0,          //  2
};

const u8 *depth3dFamilySlots(int family, int *count) {
    if (family == DEPTH3D_FAMILY_MODE27) {
        *count = (int)(sizeof(depth3dStackMode27) / sizeof(depth3dStackMode27[0]));
        return depth3dStackMode27;
    }

    *count = (int)(sizeof(depth3dStackMode01) / sizeof(depth3dStackMode01[0]));
    return depth3dStackMode01;
}
