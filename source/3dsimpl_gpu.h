
#ifndef _3DSIMPL_GPU_H_
#define _3DSIMPL_GPU_H_

#include <math.h>

#include "3dsgpu.h"
#include "3dssettings.h"

#define COMPOSE_HASH(vramAddr, pal)   ((vramAddr) << 4) + ((pal) & 0xf)

#define MAX_VERTICES                65534

// backdrop * 2, window_lr, brightness, color math
#define MAX_VERTICES_RECT           (241 * 2 + 241 + 241 + 241)
#define MAX_VERTICES_MODE7_LINE     8192
#define MAX_VERTICES_MODE7_TILE     16388

#define MAX_VERTICES_QUAD           256

#define LAYERS_COUNT 9

#define MAX_TEXTURE_POSITIONS		16383
#define MAX_HASH					(65536 * 16 / 8)

#define HDMA_PALETTE_VARIANT_CACHE_SIZE 4096

// Boundary between normal tile cache and HDMA variant pool.
// Variant pool size must be >= hash size to avoid collision.
// Boundary must be even (normal cache pairs slot p with alt-frame p^1).
#define MAX_TEXTURE_HASH_POSITIONS ((MAX_TEXTURE_POSITIONS - HDMA_PALETTE_VARIANT_CACHE_SIZE) & ~1)

typedef struct {
    s16 x, y;
} SVector2i;

typedef struct {
    s16 x, y, z;
} SVector3i;

typedef struct {
    s16 x, y, z, w;
} SVector4i;

typedef struct {
	s16 u, v;
} STexCoord2i;

typedef struct {
    float u, v;
} STexCoord2f;

typedef struct {
    float x, y, z, w;
} SVector4f;

typedef struct {
    SVector4f    Position;
	STexCoord2f  TexCoord;
	u32          Color;
	u32          Flags;
} SQuadVertex;

typedef struct {
    SVector3i    Position;
	STexCoord2i  TexCoord;
} STileVertex;

typedef struct {
    SVector2i   Position;
	u32         Color;
} SRectVertex;

// One SNES scanline of a Mode 7 plane. z is always 0, which is what tells the
// vertex shader there is no tile to unpack; w carries where that scanline falls
// between the near and far ends of the plane, so it can recede rather than stand
// up on end. See gpu3dsEncodeMode7Depth.
typedef struct {
    SVector4i    Position;
	STexCoord2f  TexCoord;
} SMode7LineVertex;

typedef struct {
    SVector4i	Position;
} SMode7TileVertex;

typedef enum 
{
    LAYER_BG0,
    LAYER_BG1,
    LAYER_BG2,
    LAYER_BG3,
    LAYER_OBJ,
    LAYER_BACKDROP,
    LAYER_COLOR_MATH, // main target
    LAYER_BRIGHTNESS, // main target
    LAYER_WINDOW_LR, // depth target
} LAYER_ID; // keep this order!

typedef enum
{
    VS_BACKDROP_SUB,    
	VS_BACKDROP_MAIN,
	VS_CLIP_TO_BLACK,
	VS_COLOR_MATH,
} VERTICAL_SECTION_ID;

typedef union {
    u64 packed;
    struct {
        u32 color;
        u32 v2; // depth for backdrop, stencil for color math
    };
} DrawableSectionValue;

typedef union {
    u16 packed;
    struct {
        u8 alphaBlending;
        u8 textureEnv;
    };
} DrawableSectionRenderState;

typedef struct 
{
	DrawableSectionValue value;
	DrawableSectionRenderState state;

    u16 	startY;
    u16 	endY;
} DrawableVerticalSection;


typedef struct 
{
    SGPURenderState     state;

    u16                 from;
    u16                 count;

    SGPU_VBO_ID         vboId;
    bool                onSub;
} SLayerSection;

typedef struct 
{
    u32             bufferOffset;

    u16             sectionsByTarget[2];
    u16             verticesByTarget[2];
    u16             sectionsTotal;
    u16             sectionsMax;
    u16             sectionsOffset;
    u16             sectionsSkipped;

    LAYER_ID        id;
    bool            m7Tile0;
} SLayer;

typedef struct
{
    SLayer          layers[LAYERS_COUNT];
    LAYER_ID        layersByTarget[2][LAYERS_COUNT];

    SLayerSection   *sections;
    void            *ibo;

    u32             sizeInBytes;

    u16             verticesTotal;
    u16             sectionsSizeInBytes;
    u16             sectionsMax;

    u8              layersTotalByTarget[2];

    bool            anythingOnSub;
    bool            hasSkippedSections;

    // true when obj and bg0-bg3 can skip indexed batching in the tiled pass
    bool            useDrawArraysForTiledLayers;
} SLayerList;

typedef struct
{
    bool            enabled;
    bool            dirty;
} SRender2xState;

//---------------------------------------------------------
// Per-layer stereoscopic parallax.
//
// The SNES composites a fixed set of hardware planes, so each
// plane can be pushed to its own depth by giving it a horizontal
// shift that is mirrored between the two eyes. The frame's vertex
// data is built once and replayed per eye with a different shift
// per layer, which keeps the SNES compositing rules (priority,
// windows, colour math) exactly as they are in 2D.
//---------------------------------------------------------
// Number of depth slots the renderer can assign. Mode 1 uses 1..13, and slot 0
// is where the backdrop, brightness and colour-math fills land -- those cover
// the whole screen, so shifting them would only open a gap and they stay put.
#define SNES_DEPTH_SLOTS    17

// How far off the side of the render target a slot is pushed while another slot
// is being previewed on its own. Wider than the target, so nothing of it lands
// back on screen.
#define DEPTH3D_ISOLATE_SHIFT   1024

// What a Mode 7 scanline's right-hand vertex carries in place of a y. The
// geometry shader reads that vertex to tell a scanline from a tile, so the y is
// a marker rather than a position; the SNES depth slot is added to it, which is
// how the right end of the scanline finds the shift the left end gets from its
// own y. Far below any y a tile or a rectangle can hold.
#define MODE7_RIGHT_EDGE_MARK   (-32768)

// The y that vertex is handed on with once the vertex shader has taken the slot
// back off it. This is what the geometry shader reads to tell a scanline from a
// tile: anything projecting below the near plane is one.
#define MODE7_GEOMETRY_MARK     (-16384)

// The fixed point the shader reads a Mode 7 scanline's distance in: it scales
// the slot's shift by 1 + w / MODE7_DEPTH_UNIT. Small enough that the gain
// below still fits the signed 16-bit field the vertex carries it in, and far
// finer than a pixel either way.
#define MODE7_DEPTH_UNIT        2048

// How much further a Mode 7 plane is pushed than the slider alone would push a
// tile layer. A slider is at most DEPTH3D_MAX pixels because that is as far as
// a flat plane can be moved before the two eyes stop fusing, but a receding
// surface spends its depth over the whole distance from the screen to the
// horizon, and the same number spread over that range barely reads at all.
//
// Note that the far end of the plane can then be moved much further than the
// edge crop is sized for, since that is bounded by the sliders. At the top of
// the slider the horizon will run off the side in one eye.
#define MODE7_DEPTH_GAIN        10

//---------------------------------------------------------
// How far a Mode 7 scanline's ground lies, as the shader
// wants it.
//
// Mode 7 states the shape of the distance itself. The plane's
// scale on a scanline is how much of the texture that scanline
// covers, and scale is one over distance, so the span a
// scanline walks is proportional to how far away the ground it
// draws is. Taking the length of the span rather than its
// horizontal part alone keeps a rotating plane steady: the two
// are equal only when the plane is square to the screen.
//
// What Mode 7 does *not* state is where any of that sits
// relative to the viewer, and that is the whole difficulty.
// Read as an absolute distance against some fixed scale, a
// plane lands wherever it happens to land: F-Zero's road
// straddles 1:1 and puts half the screen in front of the
// display, while Zelda's map sits within a few percent of it
// and gets no visible depth at all. Neither is a property of
// the game -- both are the arbitrary reference showing through.
//
// So the near end of the plane is put at the screen and the far
// end at the slot's full depth, and the 1/distance shape is
// kept in between. The slider stops meaning "how far away this
// is" and means "how much depth to spend on this plane", which
// is the useful question and the one a viewer can answer.
//
// A plane whose scale never changes has nothing between its two
// ends, and comes out flat: an overhead map, or a picture being
// spun, is not a surface receding from you and should not be
// drawn as one.
//---------------------------------------------------------
static inline float gpu3dsMode7Span(float dtx, float dty)
{
    return sqrtf(dtx * dtx + dty * dty);
}

// Where one scanline falls between the nearest and furthest the
// plane reaches this frame, as 1/distance so the spacing stays
// the one perspective actually has. 0 is the near end, 1 the far,
// and the far end is then taken out to the gain.
static inline s16 gpu3dsEncodeMode7Depth(float span, float invNear, float invRange)
{
    float f = span > 0.0f ? (invNear - 1.0f / span) * invRange : 0.0f;

    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;

    return (s16)(int)((f * MODE7_DEPTH_GAIN - 1.0f) * MODE7_DEPTH_UNIT);
}

typedef struct
{
    // Shift in SNES pixels for the right eye, per configurable slot; the left
    // eye uses the negated value. Indexed the way depthSlotSource points into
    // it, by arrangement and then DEPTH3D_SLOT.
    //
    // A shift moves the geometry of that slot alone, and every layer's geometry
    // covers the visible screen and nothing beyond it, so a displaced slot
    // simply runs out at the opposite edge and lets whatever sits behind it
    // show through. Nothing is drawn there that the game did not draw, and a
    // slot left at 0 keeps its full width whatever the others are set to.
    s8              slotShift[DEPTH3D_FAMILY_COUNT][DEPTH3D_SLOT_COUNT];

    // Which configurable slot each hardware depth slot draws its shift from, as
    // family * DEPTH3D_SLOT_COUNT + slot, or -1 for the slots nothing
    // configurable lands on. A background's two priorities land on hardware
    // slots that depend on the background mode, and the mode also says which
    // arrangement's depths apply, so both are recorded as the modes assign
    // them. Keeping the mapping rather than the resolved shifts lets the paused
    // preview follow the menu without re-rendering the frame.
    s8              depthSlotSource[SNES_DEPTH_SLOTS];

    // Bit per DEPTH3D_FAMILY the frame drew anything in, recorded as the bands
    // are rendered. A frame is normally one arrangement, but a game may change
    // mode part-way down and end up in both.
    u8              familiesDrawn;

    // Sign of the shift for the eye currently being drawn:
    // -1 = left, +1 = right, 0 = flat (no per-layer shift)
    s8              eye;

    // true when at least one layer has a non-zero shift this frame
    bool            active;

    // Whether the screen edges are cropped, resolved from
    // settings3DS.Depth3DCropEdges once a frame so the hot paths do not read
    // the setting themselves. It does two things at once: every tile is cut
    // back to the screen before the shift moves it, so a slot's strip is its
    // own shift wide and holds nothing the game kept off-screen, and the
    // finished picture then gives up the widest of those strips at all four
    // edges. Off, the tiles a game keeps just outside the screen come into
    // view as the shift carries them in.
    bool            cropEdges;

    // Whether a background's two priorities close the strip between them when
    // they sit at different depths. Resolved once a frame for the same reason.
    bool            fillGaps;

    // While >= 0, only the tiles belonging to that configurable slot are drawn:
    // every other slot is pushed off the side of the screen. Used to render the
    // menu's preview of what a slot holds, one slot at a time.
    s8              isolateSlot;

    // false when the right eye's SNES screen could not be allocated
    bool            supported;

    // the draw lists and index buffer are built once per frame and replayed
    // for the second eye
    bool            listsBuilt;
} SStereoLayerState;

typedef struct
{
    u16             vramCacheHashToTexturePosition[MAX_HASH + 1]; // 262146 bytes
    int             vramCacheTexturePositionToHash[MAX_TEXTURE_HASH_POSITIONS]; // 4*MAX_TEXTURE_HASH_POSITIONS bytes

    SLayerList      layerList;

    u32             newCacheTexturePosition;

    u16             mode7FrameCount;

    // Every Mode 7 scanline this frame emitted, in emission order, with the
    // nearest and furthest the plane reached across all of them. A frame's
    // Mode 7 can arrive in several HDMA sections, and they have to be spread
    // over one range: normalised section by section, the depth jumps back to
    // the far end at every seam.
    float           mode7Span[MAX_VERTICES_MODE7_LINE / 2];
    int             mode7Lines;
    float           mode7Near;
    float           mode7Far;

    GPU_TEXCOLOR    mode7TextureFormat;
    bool            mode7SectionsModified[4];
    bool            mode7TilesModified;

    SRender2xState  render2x;       // 512px internal SNES render path:
                                    // true hires for Mode 5, finer Mode 7 sampling, doubled geometry elsewhere
    int             renderWidth;    // 512 when render2x.enabled, else 256

    SStereoLayerState stereo;       // per-layer stereoscopic parallax
} SGPU3DSExtended;

extern SGPU3DSExtended GPU3DSExt;

void gpu3dsDeallocLayers();
void gpu3dsResetLayerSectionLimits(SLayerList *list);
void gpu3dsInitLayers();
void gpu3dsPrepareSnesScreenForNextFrame();
void gpu3dsDrawSnesScreen();
void gpu3dsUpdateStereoLayerShifts();

// Draw only the tiles belonging to one configurable depth slot, for the menu's
// per-slot previews. -1 restores the normal frame.
void gpu3dsSetDepthSlotIsolation(int slot);
void gpu3dsUpdateStereoLayerShiftsForPreview();

//---------------------------------------------------------
// Width, in SNES pixels, of the strip the finished picture gives
// up at each of its four edges when the edges are cropped.
//
// One strip stands for every slot, so it has to cover the largest
// shift in either direction. It is bounded over every slot of the
// arrangement the frame was drawn in, not over the slots the mode
// happened to use: which planes a mode has varies inside an
// arrangement -- mode 1 drops BG4, mode 6 keeps only BG1 -- and
// bounding over those would change the width of the strip, and so
// the width of the picture, every time a game changed mode. The
// two arrangements stay separate, because a depth belonging to
// the one the game is not in has no part in this frame at all. A
// frame that changes arrangement part-way down uses both.
//
// The same strip comes off both edges of both eyes, so the eye
// being drawn does not come into it. Taking only the strip each
// eye's own shifts opened would be narrower, but it would leave
// the two pictures different widths in different places, and the
// frame itself would carry a disparity nothing in the scene
// asked for.
//---------------------------------------------------------
static inline int gpu3dsGetStereoEdgeStrip()
{
    const SStereoLayerState *stereo = &GPU3DSExt.stereo;

    if (!stereo->cropEdges)
        return 0;

    int strip = 0;

    for (int family = 0; family < DEPTH3D_FAMILY_COUNT; family++) {
        if (!(stereo->familiesDrawn & (1 << family)))
            continue;

        int slotCount = 0;
        const u8 *slots = depth3dFamilySlots(family, &slotCount);

        for (int i = 0; i < slotCount; i++) {
            int shift = stereo->slotShift[family][slots[i]];

            if (shift < 0) shift = -shift;
            if (shift > strip) strip = shift;
        }
    }

    return strip;
}

void gpu3dsDrawSnesScreenForEye(gfx3dSide_t side);

//---------------------------------------------------------
// How far a background's two tile priorities pull apart, in
// pixels, when the low priority is the one sitting further back.
// Zero when the gaps between them are not being filled.
//
// Only the priority that is further back can sensibly continue
// into the gap between them, and extending the low priority is
// safe because it also draws behind the high one, so the fill
// stays hidden wherever the high priority is opaque. The
// opposite arrangement -- the high priority further back -- would
// have to paint over the low priority to fill anything, which
// would cost more than the gap does, so it is left alone.
//---------------------------------------------------------
static inline int gpu3dsGetPriorityFillWidth(int bg, int family)
{
    const SStereoLayerState *stereo = &GPU3DSExt.stereo;

    if (!stereo->active || !stereo->fillGaps)
        return 0;

    int lowShift = stereo->slotShift[family][DEPTH3D_BG_SLOT(bg, 0)];
    int highShift = stereo->slotShift[family][DEPTH3D_BG_SLOT(bg, 1)];

    return lowShift > highShift ? lowShift - highShift : 0;
}

//---------------------------------------------------------
// Records which depth slots a background's two tile priorities
// land on. Which slot that is depends on the background mode,
// so it is recorded where the modes assign it rather than
// duplicated as a table.
//---------------------------------------------------------
static inline void gpu3dsMapPlaneDepthSlots(int bg, int family, int slot0, int slot1)
{
    SStereoLayerState *stereo = &GPU3DSExt.stereo;
    int base = family * DEPTH3D_SLOT_COUNT;

    // Recorded whether or not any depth is in force: the menu's slot previews
    // need the mapping even when the feature itself is switched off.
    if (slot0 >= 0 && slot0 < SNES_DEPTH_SLOTS)
        stereo->depthSlotSource[slot0] = (s8)(base + DEPTH3D_BG_SLOT(bg, 0));

    if (slot1 >= 0 && slot1 < SNES_DEPTH_SLOTS) {
        // Depth 13 is BG3's high priority lifted to the front of the frame by
        // $2105 bit 3, which is a different place in the stack holding
        // different content, so it draws its depth from its own setting. A
        // frame that flips the bit part-way down has both, and each band then
        // gets the depth that belongs to it.
        stereo->depthSlotSource[slot1] = (s8)(base + (slot1 == 13
            ? DEPTH3D_BG3_PRIO1_FRONT
            : DEPTH3D_BG_SLOT(bg, 1)));
    }
}

//---------------------------------------------------------
// Records where the sprite priorities draw their depth from.
// Their hardware slots are the same in every mode, but which
// arrangement's depths apply is not, so this is recorded per
// band alongside the backgrounds rather than once per frame.
//---------------------------------------------------------
static inline void gpu3dsMapSpriteDepthSlots(int family)
{
    SStereoLayerState *stereo = &GPU3DSExt.stereo;
    int base = family * DEPTH3D_SLOT_COUNT;

    // Called once for every band, whatever that band draws, so this is where
    // the frame's arrangements are counted for the whole-frame edge strip.
    stereo->familiesDrawn |= (u8)(1 << family);

    for (int priority = 0; priority < 4; priority++)
        stereo->depthSlotSource[(priority + 1) * 3] = (s8)(base + DEPTH3D_OBJ_SLOT(priority));
}
void gpu3dsCommitLayerSection(SGPU_VBO_ID vboId, LAYER_ID id, SGPURenderState *state, bool sub = false, bool reuseVertices = false);

void gpu3dsSetMode7TexturesPixelFormat(GPU_TEXCOLOR fmt);

void gpu3dsInitializeMode7Vertexes();

void gpu3dsAddQuadRect(float x0, float y0, float x1, float y1, u16 wx, u16 wy, int z, u32 fillColor, u32 borderColor = 0, u8 borderSize = 0);

inline u16 __attribute__((always_inline)) gpu3dsGetValueWithinLimit(u16 value, u32 from, u32 max) {
    return (from + value > max) ? (max - from) : value;
}

inline void __attribute__((always_inline)) gpu3dsAddQuadVertexes(
    float x0, float y0, float x1, float y1,
    STexCoord2f tl, STexCoord2f tr, STexCoord2f bl, STexCoord2f br,
    float z, int color = 0)
{
    SVertexList *list = &GPU3DS.vertices[VBO_SCREEN];
    SQuadVertex *vertices = &((SQuadVertex *) list->data)[list->from + list->count];

	vertices[0].Position = {x0, y0, z, 1};
	vertices[1].Position = {x1, y0, z, 1};
	vertices[2].Position = {x0, y1, z, 1};

	vertices[3].Position = {x1, y1, z, 1};
	vertices[4].Position = {x0, y1, z, 1};
	vertices[5].Position = {x1, y0, z, 1};

	vertices[0].TexCoord = tl;
	vertices[1].TexCoord = tr;
	vertices[2].TexCoord = bl;

	vertices[3].TexCoord = br;
	vertices[4].TexCoord = bl;
	vertices[5].TexCoord = tr;

	u32 colorSwapped = __builtin_bswap32(color);

    vertices[0].Color = colorSwapped;
    vertices[1].Color = colorSwapped;
    vertices[2].Color = colorSwapped;

    vertices[3].Color = colorSwapped;
    vertices[4].Color = colorSwapped;
    vertices[5].Color = colorSwapped;

    list->count += 6;
}

inline void __attribute__((always_inline)) gpu3dAddSubTextureQuadVertexes(
    float x0, float y0, float x1, float y1,
    const Tex3DS_SubTexture* subTex, int origWidth, int origHeight, int textureWidth, int textureHeight,
    float z, u32 color)
{
    if (!Tex3DS_SubTextureRotated(subTex)) {
        float top_u, top_v, bot_u, bot_v;
        Tex3DS_SubTextureTopLeft(subTex, &top_u, &top_v);
        Tex3DS_SubTextureBottomLeft(subTex, &bot_u, &bot_v);

        float tx0 = top_u * textureWidth;
        float ty0 = top_v * textureHeight;

        // If Top > Bottom, we are traversing memory backwards (common in 3DS)
        float dirY = (bot_v * textureHeight > ty0) ? 1.0f : -1.0f;
        float tx1 = tx0 + origWidth;
        float ty1 = ty0 + (origHeight * dirY);

        STexCoord2f tl = { tx0, ty0 };
        STexCoord2f tr = { tx1, ty0 };
        STexCoord2f bl = { tx0, ty1 };
        STexCoord2f br = { tx1, ty1 };

        gpu3dsAddQuadVertexes(x0, y0, x1, y1, tl, tr, bl, br, z, color);
    }
    else {
        float tl_u, tl_v, tr_u, tr_v, bl_u, bl_v, br_u, br_v;
        Tex3DS_SubTextureTopLeft(subTex, &tl_u, &tl_v);
        Tex3DS_SubTextureTopRight(subTex, &tr_u, &tr_v);
        Tex3DS_SubTextureBottomLeft(subTex, &bl_u, &bl_v);
        Tex3DS_SubTextureBottomRight(subTex, &br_u, &br_v);

        STexCoord2f tl = { tl_u * textureWidth, tl_v * textureHeight };
        STexCoord2f tr = { tr_u * textureWidth, tr_v * textureHeight };
        STexCoord2f bl = { bl_u * textureWidth, bl_v * textureHeight };
        STexCoord2f br = { br_u * textureWidth, br_v * textureHeight };

        gpu3dsAddQuadVertexes(x0, y0, x1, y1, tl, tr, bl, br, z, color);
    }
}

inline void __attribute__((always_inline)) gpu3dsAddSimpleQuadVertexes(
    float x0, float y0, float x1, float y1,
    float tx0, float ty0, float tx1, float ty1,
    float z, int color = 0)
{
    STexCoord2f tl = {tx0, ty0};
    STexCoord2f tr = {tx1, ty0};
    STexCoord2f bl = {tx0, ty1};
    STexCoord2f br = {tx1, ty1};

    gpu3dsAddQuadVertexes(x0, y0, x1, y1, tl, tr, bl, br, z, color);
}

inline void __attribute__((always_inline)) gpu3dsAddRectangleVertexes(s16 x0, s16 y0, s16 x1, s16 y1, u32 color)
{
    SVertexList *list = &GPU3DS.vertices[VBO_SCENE_RECT];
    SRectVertex *vertices = &((SRectVertex *) list->data)[list->from + list->count];

    // using -1 for non-tile detection in shader
    vertices[0].Position = (SVector2i){x0, y0};
    vertices[1].Position = (SVector2i){x1, y1};

    u32 swappedColor = __builtin_bswap32(color);
    vertices[0].Color = swappedColor;
    vertices[1].Color = swappedColor;

    list->count += 2;
}


//---------------------------------------------------------
// Cuts a tile quad back to the visible screen when a slot may
// be shifted for stereo depth.
//
// Geometry outside the screen is normally thrown away by the
// rasterizer, so the paths above are free to run a tile past the
// edge. A shift moves that geometry sideways, which would carry
// it into view -- and what a game keeps just outside the screen
// is its own business. Worse, how far a tile hangs over the edge
// depends on the layer's scroll, which HDMA can change from one
// band of the frame to the next, so the strip a shifted slot
// leaves would step in and out down the screen.
//
// Cut to the screen instead. The strip is then exactly as wide
// as the slot's own shift, straight down the frame, and holds
// whatever sits behind that slot.
//---------------------------------------------------------
static inline bool gpu3dsClipTileToScreen(s16 &x0, s16 &x1, s16 &tx0, s16 &tx1)
{
    s16 width = (s16)GPU3DSExt.renderWidth;

    if (x0 >= 0 && x1 <= width)
        return true;

    if (x1 <= 0 || x0 >= width)
        return false;

    int dx = x1 - x0;
    int dtx = tx1 - tx0;

    // Rounded, because a quad does not always carry one texel per pixel: a
    // mosaic block stretches a single texel over the whole block, and the
    // hi-res tile path packs seven texels into four pixels.
    if (x0 < 0) {
        tx0 = (s16)(tx0 + (-x0 * dtx + dx / 2) / dx);
        x0 = 0;
    }

    if (x1 > width) {
        tx1 = (s16)(tx1 - ((x1 - width) * dtx + dx / 2) / dx);
        x1 = width;
    }

    return true;
}

inline void __attribute__((always_inline)) gpu3dsAddTileVertexes(
    s16 x0, s16 y0, s16 x1, s16 y1,
    s16 tx0, s16 ty0, s16 tx1, s16 ty1,
    s16 z)
{
    if (GPU3DSExt.stereo.cropEdges && !gpu3dsClipTileToScreen(x0, x1, tx0, tx1))
        return;

    SVertexList *list = &GPU3DS.vertices[VBO_SCENE_TILE];
    STileVertex *vertices = &((STileVertex *) list->data)[list->from + list->count];

    vertices[0].Position = (SVector3i){x0, y0, z};
    vertices[1].Position = (SVector3i){x1, y1, z};

    vertices[0].TexCoord = (STexCoord2i){tx0, ty0};
    vertices[1].TexCoord = (STexCoord2i){tx1, ty1};

    list->count += 2;
}

//---------------------------------------------------------
// Adds one scanline of a Mode 7 plane.
//
// The two ends are not symmetric. The left one carries the
// scanline's y and the SNES depth it draws at, the way a tile
// does; the right one carries a marker in place of a y,
// because the geometry shader tells a scanline from a tile by
// looking at it. That leaves the right end with no depth of
// its own, so the slot rides in the marker: without it only
// the left end would take the slot's stereo shift and the
// scanline would stretch instead of moving.
//---------------------------------------------------------
inline void __attribute__((always_inline)) gpu3dsAddMode7LineVertexes(
    s16 x0, s16 y0, s16 x1, s16 slot, s16 depth7,
    float tx0, float ty0, float tx1, float ty1)
{
    SVertexList *list = &GPU3DS.vertices[VBO_SCENE_MODE7_LINE];
    SMode7LineVertex *vertices = (SMode7LineVertex *) list->data + list->from + list->count;

    vertices[0].Position = (SVector4i){x0, y0, 0, depth7};
    vertices[1].Position = (SVector4i){x1, (s16)(MODE7_RIGHT_EDGE_MARK + slot), 0, depth7};

    vertices[0].TexCoord = {tx0, ty0};
    vertices[1].TexCoord = {tx1, ty1};

    list->count += 2;
}


//---------------------------------------------------------
// Records how far one Mode 7 scanline reaches, as it is
// emitted. The depth cannot be worked out yet: where a scanline
// falls between the two ends of the plane is not known until
// every section of it has been walked.
//---------------------------------------------------------
inline void gpu3dsRecordMode7Span(float span)
{
    if (GPU3DSExt.mode7Lines >= (int)(sizeof(GPU3DSExt.mode7Span) / sizeof(float)))
        return;

    GPU3DSExt.mode7Span[GPU3DSExt.mode7Lines++] = span;

    if (span < GPU3DSExt.mode7Near) GPU3DSExt.mode7Near = span;
    if (span > GPU3DSExt.mode7Far)  GPU3DSExt.mode7Far = span;
}

inline void gpu3dsResetMode7Spans()
{
    GPU3DSExt.mode7Lines = 0;
    GPU3DSExt.mode7Near = 1e30f;
    GPU3DSExt.mode7Far = 0.0f;
}

//---------------------------------------------------------
// Gives every Mode 7 scanline of the finished frame its place
// between the plane's near and far ends. Run once the whole
// frame has been built, and before either eye is drawn.
//
// A plane whose scale never changes has no two ends to spread
// anything between, so it is left flat and takes its slot's
// depth whole. That is the overhead map and the spinning
// picture: not a surface receding from the viewer.
//---------------------------------------------------------
inline void gpu3dsApplyMode7LineDepths()
{
    int lines = GPU3DSExt.mode7Lines;
    float near = GPU3DSExt.mode7Near;
    float far = GPU3DSExt.mode7Far;

    if (lines <= 0 || near <= 0.0f || far <= 0.0f)
        return;

    float invNear = 1.0f / near;
    float range = invNear - 1.0f / far;

    // Under about a percent of reach and the plane is flat as far as anyone can
    // see; spreading a whole slider over that would only turn rounding in the
    // matrix into depth that is not there.
    if (range < invNear * 0.01f)
        return;

    float invRange = 1.0f / range;
    // From the start of the buffer, not from list->from: that has been walked
    // forward past every section as they were committed, and all of this
    // frame's scanlines were appended from index 0 in the order they were
    // recorded. Nothing else shares this list.
    SVertexList *list = &GPU3DS.vertices[VBO_SCENE_MODE7_LINE];
    SMode7LineVertex *vertices = (SMode7LineVertex *) list->data;

    for (int i = 0; i < lines; i++)
    {
        s16 w = gpu3dsEncodeMode7Depth(GPU3DSExt.mode7Span[i], invNear, invRange);

        vertices[i * 2 + 0].Position.w = w;
        vertices[i * 2 + 1].Position.w = w;
    }
}

inline void __attribute__((always_inline)) gpu3dsSetMode7TileModified(int idx, u8 data)
{
    SMode7TileVertex *m7vertices = &((SMode7TileVertex *)GPU3DS.vertices[VBO_MODE7_TILE].data) [idx];

    m7vertices[0].Position.w = GPU3DSExt.mode7FrameCount;
    m7vertices[0].Position.z = data;

    GPU3DSExt.mode7TilesModified = true;
    GPU3DSExt.mode7SectionsModified[idx >> 12] = true;
}

#endif
