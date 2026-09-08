
#include "snes9x.h"
#include "ppu.h"

#include <3ds.h>
#include "3dsgpu.h"
#include "3dsimpl.h"
#include "3dsimpl_gpu.h"
#include "3dslog.h"

SGPU3DSExtended GPU3DSExt;

void gpu3dsDeallocLayers()
{   
    SLayerList *list = &GPU3DSExt.layerList;

    if (list == nullptr)
        return;

    linearFree(list->sections);
    linearFree(list->ibo);
}

void gpu3dsResetLayer(SLayer *layer) {
    layer->sectionsByTarget[TARGET_SNES_MAIN] = 0;
    layer->verticesByTarget[TARGET_SNES_MAIN] = 0;

    layer->sectionsByTarget[TARGET_SNES_SUB] = 0;
    layer->verticesByTarget[TARGET_SNES_SUB] = 0;
    
    layer->sectionsTotal = 0;
    layer->m7Tile0 = false;
}

void gpu3dsResetLayers(SLayerList *list) {
    list->verticesTotal = 0;
            
    for (int i = 0; i < LAYERS_COUNT; i++) {
        gpu3dsResetLayer(&list->layers[i]);
    }
}

// reset to initial state when loading a game
void gpu3dsResetLayerSectionLimits(SLayerList *list) {
    list->sectionsMax = 0;

    for (int i = 0; i < LAYERS_COUNT; i++) 
    {
        SLayer *layer = &list->layers[i];

        layer->sectionsOffset = list->sectionsMax;

        switch (i) {
            case LAYER_WINDOW_LR:
            case LAYER_BRIGHTNESS:
                layer->sectionsMax = 1; // always 0-1 section
                break;
            case LAYER_BACKDROP:
                layer->sectionsMax = 2; // always 0-2 sections
                break;
            default:
                layer->sectionsMax = 128; 
                break;
        }

        list->sectionsMax += layer->sectionsMax;
    }
}

void gpu3dsAdjustLayerSectionLimits(SLayerList *list) {
    u16 threshold = 16;
    u16 allowedSectionsMax = list->sectionsSizeInBytes / sizeof(SLayerSection);
    u16 newSectionsMax = 0;

    for (int i = 0; i < LAYERS_COUNT; i++) {
        SLayer *layer = &list->layers[i];

        layer->sectionsOffset = newSectionsMax;

        if (layer->sectionsSkipped) {
            layer->sectionsMax += (layer->sectionsSkipped < threshold ? threshold : layer->sectionsSkipped);
        }
        
        newSectionsMax += layer->sectionsMax;
    }

    if (newSectionsMax <= allowedSectionsMax) {
        list->sectionsMax = newSectionsMax;

        return;
    }

    // if newSectionsMax > allowedSectionsMax, we have to reduce layer->sectionsMax for !layer->sectionsSkipped layers
    // so that newSectionsMax still fits into the allocated memory
    // (e.g. color math layer has sectionsMax = 128 but only 7 sections are needed for the current frame)
    //
    // for convinience we only do this for color math,obj and bg0-bg3 in prioritized order
    // because those layers will have the highest number of unused sections
    u16 sectionsToReduce = newSectionsMax - allowedSectionsMax;
    u16 reducedSections = 0;

    LAYER_ID order[6] = {
        LAYER_COLOR_MATH,
        LAYER_OBJ,
        LAYER_BG3,
        LAYER_BG2,
        LAYER_BG1,
        LAYER_BG0,
    };

    for (int i = 0; i < 6; i++) {
        SLayer *layer = &list->layers[order[i]];

        if (!layer->sectionsSkipped) {
            u16 newMax = layer->sectionsTotal < threshold ? threshold : layer->sectionsTotal;
            reducedSections += layer->sectionsMax - newMax;
            layer->sectionsMax = newMax;
        }

        if (sectionsToReduce <= reducedSections)
            break;
    }

    newSectionsMax = 0;
    for (int i = 0; i < LAYERS_COUNT; i++) {
        SLayer *layer = &list->layers[i];

        // just in case if we still exceed the max limit (should never happen)
        if (newSectionsMax + layer->sectionsMax > allowedSectionsMax) {
            layer->sectionsMax = 0;
        }

        layer->sectionsOffset = newSectionsMax;

        if (layer->sectionsSkipped) {
            layer->sectionsSkipped = false;
        }

        newSectionsMax += layer->sectionsMax;
    }

    list->sectionsMax = newSectionsMax;
}

u64 gpu3dsGetLayerPackedMask(LAYER_ID id, bool firstSection) {
    if (id == LAYER_OBJ)
    {
        return firstSection
            ? PACKED_MASK_TEX_BIND | PACKED_MASK_STENCIL | PACKED_MASK_ALPHA_TEST | PACKED_MASK_TEX_OFFSET
            : PACKED_MASK_STENCIL;
    }

    if (id == LAYER_BG0 || id == LAYER_BG1)
    {
        return PACKED_MASK_TEX_BIND
            | PACKED_MASK_STENCIL
            | PACKED_MASK_ALPHA_TEST
            | PACKED_MASK_TEX_OFFSET;
    }

    if (id == LAYER_BG2 || id == LAYER_BG3)
    {
        return firstSection
            ? PACKED_MASK_TEX_BIND | PACKED_MASK_STENCIL | PACKED_MASK_ALPHA_TEST | PACKED_MASK_TEX_OFFSET
            : PACKED_MASK_STENCIL;
    }

    return 0;
}

void gpu3dsInitLayers() {
    SLayerList *list = &GPU3DSExt.layerList;

    // Sized by index references, not unique vertices.
    // A tile drawn on both sub and main is referenced twice, so worst case is 2x MAX_VERTICES.
    list->sizeInBytes = gpu3dsGetNextPowerOf2(2 * MAX_VERTICES * sizeof(u16));
    list->ibo = linearAlloc(list->sizeInBytes);

    gpu3dsResetLayers(list);

    for (int i = 0; i < LAYERS_COUNT; i++) 
    {
        LAYER_ID id = LAYER_ID(i);
        SLayer *layer = &list->layers[id];

        layer->id = id;
    }

    list->sectionsSizeInBytes = gpu3dsGetNextPowerOf2(list->sectionsMax * sizeof(SLayerSection));
    list->sections = (SLayerSection *)linearAlloc(list->sectionsSizeInBytes);
			
    log3dsWrite("ibo size: %dkb, sections size: %dkb",
        list->sizeInBytes / 1024,
        list->sectionsSizeInBytes / 1024
    );
}

int compareSections(const SLayerSection *a, const SLayerSection *b, bool tile0) {
    // First, compare target: TARGET_SNES_SUB should come first
    if (a->onSub != b->onSub) {
        return a->onSub ? -1 : 1;
    }
    
    // If targets are equal, compare texture: SNES_MODE7_TILE_0 should come first
    if (tile0 && a->state.textureBind != b->state.textureBind) {
        return (a->state.textureBind == SNES_MODE7_TILE_0) ? -1 : 1;
    }

    return 0;
}

void sortSections(SLayerSection *sections, int n, bool tile0) {
    for (int i = 1; i < n; i++) {
        SLayerSection section = sections[i];
        int j = i - 1;
        while (j >= 0 && compareSections(&section, &sections[j], tile0) < 0) {
            sections[j + 1] = sections[j];
            j--;
        }
        sections[j + 1] = section;
    }
}

// window_lr, backdrop, color math, brightness
void gpu3dsDrawVerticalSectionLayer(SLayer *layer, int from, int to) {
    SLayerList *list = &GPU3DSExt.layerList;

    u64 mask = PACKED_MASK_TEX_ENV
        | PACKED_MASK_STENCIL
        | PACKED_MASK_ALPHA_TEST
        | PACKED_MASK_ALPHA_BLEND;

    if (layer->id == LAYER_COLOR_MATH)
        mask |= PACKED_MASK_TEX_BIND;

    for (int i = from; i < to; i++) {
        SLayerSection *section = &list->sections[i];

        GPU3DS.currentRenderState.packed =
            (GPU3DS.currentRenderState.packed & ~mask) | (section->state.packed & mask);
        gpu3dsDraw(&GPU3DS.vertices[section->vboId], NULL, section->count, section->from);
    }
}

// obj, bg0-bg3 - single section per target.
// Only used when list->useDrawArraysForTiledLayers holds, because mixing
// DrawArrays and DrawElements on the OBJ/BG pass seems to freeze real hardware
// (SMK race).
void gpu3dsDrawTiledLayerSingleSection(SLayer *layer, SLayerSection *section) {
    GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0_COLOR_ALPHA;
    GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_DISABLED;

    u64 mask = gpu3dsGetLayerPackedMask(layer->id, true);
    GPU3DS.currentRenderState.packed =
        (GPU3DS.currentRenderState.packed & ~mask) | (section->state.packed & mask);

    gpu3dsDraw(&GPU3DS.vertices[section->vboId], NULL, section->count, section->from);
}

// obj, bg0-bg3
void gpu3dsDrawTiledLayer(SLayer *layer, u16 *indices, int from, int to, bool buildIndices) {
    SLayerList *list = &GPU3DSExt.layerList;
    u16 batchFrom = 0;
    u16 batchCount = 0;

    // Those fields are set once before the loop and stay constant 
    GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0_COLOR_ALPHA;
    GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_DISABLED;
    
    // restrict the diff to only the properties that actually vary across sections for that layer type
    u64 layerMask[2];
    layerMask[0] = gpu3dsGetLayerPackedMask(layer->id, true);  // first section
    layerMask[1] = gpu3dsGetLayerPackedMask(layer->id, false); // subsequent sections

    // init from first section
    SLayerSection *first = &list->sections[from];
    SGPU_VBO_ID vboId = first->vboId;
    GPU3DS.currentRenderState.packed =
        (GPU3DS.currentRenderState.packed & ~layerMask[0]) | (first->state.packed & layerMask[0]);

    for (int idx = from; idx < to; idx++) {
        SLayerSection *section = &list->sections[idx];
        u16 sFrom = section->from;
        u16 sCount = section->count;

        // batch break on state change
        if (idx > from) {
            bool changed = ((GPU3DS.currentRenderState.packed ^ section->state.packed) & layerMask[1])
                || section->vboId != vboId;

            if (changed) {
                gpu3dsDraw(&GPU3DS.vertices[vboId], (void *)(indices + batchFrom), batchCount);
                vboId = section->vboId;
                GPU3DS.currentRenderState.packed =
                    (GPU3DS.currentRenderState.packed & ~layerMask[1]) | (section->state.packed & layerMask[1]);
                batchFrom += batchCount;
                batchCount = 0;
            }
        }

        // build sequential indices (the second eye replays the same buffer)
        if (buildIndices) {
            u16 *dst = indices + batchFrom + batchCount;
            int i = 0;
            for (; i <= sCount - 4; i += 4) {
                dst[i]     = sFrom + i;
                dst[i + 1] = sFrom + i + 1;
                dst[i + 2] = sFrom + i + 2;
                dst[i + 3] = sFrom + i + 3;
            }
            for (; i < sCount; i++) {
                dst[i] = sFrom + i;
            }
        }

        batchCount += sCount;
    }

    // draw final batch
    gpu3dsDraw(&GPU3DS.vertices[vboId], (void *)(indices + batchFrom), batchCount);
}

void gpu3dsDrawLayers(SLayerList *list, bool buildIndices) {
    // draw window_lr into depth buffer first
    SLayer *layer = &list->layers[LAYER_WINDOW_LR];

    if (layer->verticesByTarget[0]) {
        GPU3DS.currentRenderState.target = TARGET_SNES_DEPTH;

        gpu3dsDrawVerticalSectionLayer(layer, layer->sectionsOffset, layer->sectionsOffset + layer->sectionsByTarget[TARGET_SNES_MAIN]);
    }

    u8 i0 = list->anythingOnSub ? 1 : 0;

    for (int i = i0; i >= 0; i--) {
        GPU3DS.currentRenderState.target = (SGPU_TARGET_ID)i;

        bool sub = i == TARGET_SNES_SUB;

        for (int j = 0; j < list->layersTotalByTarget[i]; j++) {
            LAYER_ID id = list->layersByTarget[i][j];
            SLayer *layer = &list->layers[id];

            int from = layer->sectionsOffset + (sub ? 0 : layer->sectionsByTarget[TARGET_SNES_SUB]);
            int to = from + layer->sectionsByTarget[i];

            if (to <= from) continue;

            // While one slot is being previewed on its own, the layers that are
            // not tiles -- backdrop, colour math, brightness -- are left out.
            // They are drawn as plain rectangles carrying no slot, so they
            // cannot be held back the way the tiles can, and the backdrop alone
            // would cover every preview.
            bool backdropOnly = GPU3DSExt.stereo.isolateSlot == DEPTH3D_SLOT_COUNT;

            if (GPU3DSExt.stereo.isolateSlot >= 0 && (id >= LAYER_BACKDROP) != backdropOnly)
                continue;

            GPU3DS.currentRenderState.depthTest = id < LAYER_OBJ ? SGPU_STATE_ENABLED : SGPU_STATE_DISABLED;

            if (id < LAYER_BACKDROP) {
                if (list->useDrawArraysForTiledLayers) {
                    gpu3dsDrawTiledLayerSingleSection(layer, &list->sections[from]);
                }
                else {
                    u32 bufferOffset = layer->bufferOffset + (sub ? 0 : layer->verticesByTarget[TARGET_SNES_SUB]);
                    u16 *indices = (u16 *)list->ibo + bufferOffset;
                    gpu3dsDrawTiledLayer(layer, indices, from, to, buildIndices);
                }
            }
            else {
                gpu3dsDrawVerticalSectionLayer(layer, from, to);
            }
        }
    }
}


void gpu3dsDrawMode7Texture()
{
    if (!IPPU.Mode7Prepared || !GPU3DSExt.mode7TilesModified) return;

	t3dsStartTimer(TIMER_DRAW_M7_TEXTURE);
	gpu3dsSetMode7TexturesPixelFormat(IPPU.Mode7EXTBGFlag ? GPU_RGBA4 : GPU_RGBA5551);

	GPU3DS.currentRenderState.textureBind = SNES_MODE7_TILE_CACHE;
	GPU3DS.currentRenderState.shader = SPROGRAM_MODE7;
	GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0;
	GPU3DS.currentRenderState.stencilTest = STENCIL_TEST_DISABLED;
	GPU3DS.currentRenderState.alphaTest = ALPHA_TEST_DISABLED;
	GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_DISABLED;

	SVertexList *list = &GPU3DS.vertices[VBO_MODE7_TILE];
    SGPUTexture *texture = &GPU3DS.textures[SNES_MODE7_FULL];

    // 3DS does not allow rendering to a viewport whose width > 512
    // so our 1024x1024 texture is split into 4 512x512 parts
	GPU3DS.currentRenderState.target = TARGET_SNES_MODE7_FULL;

	for (int section = 0; section < 4; section++)
	{
		if (GPU3DSExt.mode7SectionsModified[section])
		{
			GPU3DSExt.mode7SectionsModified[section] = false;

			// invalidate so next gpu3dsApplyRenderState re-applies the target (framebuf address changes per section)
			GPU3DS.appliedRenderState.target = TARGET_UNSET;
			int addressOffset = ((3 - section) * 0x40000) * gpu3dsGetPixelSize(texture->tex.fmt);
    		texture->target->frameBuf.colorBuf = (void *)((int)texture->tex.data + addressOffset);

			gpu3dsDraw(list, NULL, 4096, 4096 * section);
		}
	}

	GPU3DSExt.mode7TilesModified = false;

	// Citra workaround:
    // without this, drawing all 4 sections above leaves the Mode 7 texture blank on Citra.
	// Force Citra to flush the surface to memory.
	//
	// The whole surface, not a token 16 bytes: what the copy flushes is what
	// comes back, and a game that writes its Mode 7 tilemap once and never
	// again -- a still picture, or anything that fills VRAM at boot -- has no
	// second bake to correct the rest, so its plane stays blank for good.
	// Only emulators reach this, and only on a frame that changed a tile.
	if (!GPU3DS.isReal3DS)
	{
		C3D_SyncTextureCopy(
			(u32 *)texture->tex.data, 0,
			(u32 *)texture->tex.data, 0,
			texture->tex.width * texture->tex.height * gpu3dsGetPixelSize(texture->tex.fmt), 8);
	}

	// SNES_MODE7_TILE_0 is only sampled when Mode7Repeat is 3 ("repeat tile 0
	// across the plane"). Modes 0 and 2 (wrap / fill with colour 0) don't
	// sample it. Skipping the bake saves one draw + one render-target switch
	// every Mode 7 frame on those modes.
	if (PPU.Mode7Repeat == 3)
	{
		GPU3DS.currentRenderState.target = TARGET_SNES_MODE7_TILE_0;
		gpu3dsDraw(list, NULL, 4, 16384);
	}

	// re-bind our tile shader
	GPU3DS.currentRenderState.shader = SPROGRAM_TILES;

	t3dsStopTimer(TIMER_DRAW_M7_TEXTURE);

    gpu3dsIncrementMode7UpdateFrameCount();
}

void gpu3dsPrepareSnesScreenForNextFrame() {
    SLayerList *list = &GPU3DSExt.layerList;

    if (list->hasSkippedSections) {
        gpu3dsAdjustLayerSectionLimits(list);
        list->hasSkippedSections = false;
    }
    
    gpu3dsResetLayers(list);

    GPU3DSExt.stereo.listsBuilt = false;
    gpu3dsUpdateStereoLayerShifts();

    // flip snes VBOs to the alternate half of the buffer
    // make sure this is called BEFORE S9xMainLoop so that vertex writes go to different memory
	gpu3dsPrepareListForNextFrame(&GPU3DS.vertices[VBO_SCENE_RECT], true);
	gpu3dsPrepareListForNextFrame(&GPU3DS.vertices[VBO_SCENE_TILE], true);
	gpu3dsPrepareListForNextFrame(&GPU3DS.vertices[VBO_SCENE_MODE7_LINE], true);
	gpu3dsResetMode7Spans();

    if (GPU3DSExt.render2x.dirty) {
        if (GPU3DSExt.stereo.supported)
            gpu3dsClearTexture(&GPU3DS.textures[SNES_MAIN_RIGHT], 0);
        gpu3dsClearTexture(&GPU3DS.textures[SNES_MAIN], 0);
        gpu3dsClearTexture(&GPU3DS.textures[SNES_SUB], 0);
        gpu3dsClearTexture(&GPU3DS.textures[SNES_DEPTH], 0);
        GPU3DSExt.render2x.dirty = false;
    }
}

static void gpu3dsUploadDepthSlotOffsets();

//---------------------------------------------------------
// Groups this frame's committed sections by render target and
// assigns each tiled layer its slice of the index buffer.
// Deterministic, so the second (right-eye) pass reuses the result.
//---------------------------------------------------------
static void gpu3dsBuildSnesLayerDrawLists(SLayerList *list) {
    list->anythingOnSub = false;
    list->useDrawArraysForTiledLayers = true;
    list->layersTotalByTarget[TARGET_SNES_SUB] = 0;
    list->layersTotalByTarget[TARGET_SNES_MAIN] = 0;

    LAYER_ID drawOrder[8] = {
        LAYER_BACKDROP,
        LAYER_OBJ,
        LAYER_BG0,
        LAYER_BG1,
        LAYER_BG2,
        LAYER_BG3,
        LAYER_COLOR_MATH,
        LAYER_BRIGHTNESS,
    };

    u32 bufferOffset = 0;
    
    for (int i = 0; i < 8; i++) {
        LAYER_ID id = drawOrder[i];

        SLayer *layer = &list->layers[id];
        
        u16 verticesOnSub = layer->verticesByTarget[TARGET_SNES_SUB];
        u16 verticesOnMain = layer->verticesByTarget[TARGET_SNES_MAIN];

        int verticesTotal = verticesOnSub + verticesOnMain;

        if (!verticesTotal) {
            continue;
        }

        if (verticesOnMain) {
            list->layersByTarget[TARGET_SNES_MAIN][list->layersTotalByTarget[TARGET_SNES_MAIN]++] = id;
        }

        if (verticesOnSub) {
            list->layersByTarget[TARGET_SNES_SUB][list->layersTotalByTarget[TARGET_SNES_SUB]++] = id;
            list->anythingOnSub = true;
        }


        if ((verticesOnMain && verticesOnSub) || layer->m7Tile0) {
            sortSections(list->sections + layer->sectionsOffset, layer->sectionsTotal, layer->m7Tile0);
        }
        
        if (id < LAYER_BACKDROP)
        {
            if (layer->sectionsByTarget[TARGET_SNES_MAIN] > 1 || layer->sectionsByTarget[TARGET_SNES_SUB] > 1)
                list->useDrawArraysForTiledLayers = false;

            layer->bufferOffset = bufferOffset;
            bufferOffset += verticesTotal;
        }
    }

}

void gpu3dsDrawSnesScreen() {
    SLayerList *list = &GPU3DSExt.layerList;

    if (!list->verticesTotal || list->hasSkippedSections)
        return;

    bool firstPass = !GPU3DSExt.stereo.listsBuilt;

    if (firstPass) {
        gpu3dsBuildSnesLayerDrawLists(list);
        GPU3DSExt.stereo.listsBuilt = true;
    }

    // The SNES layers are tile geometry whatever the caller was drawing before.
    // The pause menu in particular begins its frame on the screen shader, and
    // the per-layer draws below only ever set the target, not the program.
    GPU3DS.currentRenderState.shader = SPROGRAM_TILES;

    // Every Mode 7 scanline of the frame is in by now, so the plane's reach is
    // known and each of them can be given its place along it.
    gpu3dsApplyMode7LineDepths();

    gpu3dsUploadDepthSlotOffsets();

    // Safe to leave in while a slot is previewed on its own: it only bakes the
    // Mode 7 tile texture, which the plane samples but which is not the plane,
    // and a paused frame has no modified tiles for it to bake anyway.
    gpu3dsDrawMode7Texture();

    gpu3dsDrawLayers(list, firstPass);
}

//---------------------------------------------------------
// Recomputes the shifts for the preview shown while paused, so
// the menu's sliders can be judged against the frame on screen.
//---------------------------------------------------------
void gpu3dsUpdateStereoLayerShiftsForPreview() {
    SStereoLayerState *stereo = &GPU3DSExt.stereo;
    s8 previousSource[SNES_DEPTH_SLOTS];
    u8 previousFamilies = stereo->familiesDrawn;

    memcpy(previousSource, stereo->depthSlotSource, sizeof(previousSource));

    gpu3dsUpdateStereoLayerShifts();

    // Both are recorded while rendering, which is not happening now.
    memcpy(stereo->depthSlotSource, previousSource, sizeof(previousSource));
    stereo->familiesDrawn = previousFamilies;
}

//---------------------------------------------------------
// Hands the tile shader the horizontal shift for every depth
// slot, so each plane-and-priority can sit at its own depth
// without splitting the frame into more draw calls.
//---------------------------------------------------------
static void gpu3dsUploadDepthSlotOffsets() {
    const SStereoLayerState *stereo = &GPU3DSExt.stereo;
    int loc = GPU3DS.shaderULocs[ULOC_SLOT_OFFSET];

    if (loc < 0)
        return;

    for (int slot = 0; slot < SNES_DEPTH_SLOTS; slot++) {
        int source = stereo->depthSlotSource[slot];
        float shift;

        if (stereo->isolateSlot >= 0) {
            // Everything but the slot being previewed is pushed clear of the
            // render target, which leaves the one slot alone on the screen.
            // The slot is matched whichever arrangement drew it, so a preview
            // shows what is on it now rather than only in one of the two.
            bool isolated = source >= 0
                && (source % DEPTH3D_SLOT_COUNT) == stereo->isolateSlot;

            shift = isolated ? 0.0f : (float)DEPTH3D_ISOLATE_SHIFT;
        } else {
            shift = source >= 0
                ? (float)(stereo->eye * stereo->slotShift[source / DEPTH3D_SLOT_COUNT][source % DEPTH3D_SLOT_COUNT])
                : 0.0f;
        }

        C3D_FVUnifSet(GPU_VERTEX_SHADER, loc + slot, shift, 0.0f, 0.0f, 0.0f);
    }

    // How much of a Mode 7 scanline's own distance the shader reads. Zero
    // leaves every scanline of the plane at the slot's depth, which is what the
    // perspective switch turns off and what an isolated slot needs: the shift
    // that holds a slot off the screen is scaled by this too, and a scanline
    // sitting near the screen plane would scale it to nothing and stay in view.
    bool perspective = settings3DS.Depth3DMode7Perspective && stereo->isolateSlot < 0;

    C3D_FVUnifSet(GPU_VERTEX_SHADER, GPU3DS.shaderULocs[ULOC_MODE7_DEPTH],
        perspective ? 1.0f / MODE7_DEPTH_UNIT : 0.0f,
        -(float)MODE7_RIGHT_EDGE_MARK,
        (float)MODE7_GEOMETRY_MARK,
        0.0f);
}

//---------------------------------------------------------
// Draws the whole SNES frame for one eye into that eye's screen
// texture. The vertex data is built once per frame during
// emulation and replayed here, with each layer shifted
// horizontally by its stereo parallax.
//---------------------------------------------------------
void gpu3dsDrawSnesScreenForEye(gfx3dSide_t side) {
    GPU3DSExt.stereo.eye = GPU3DSExt.stereo.active ? (side == GFX_RIGHT ? 1 : -1) : 0;
    GPU3DS.snesSide = GPU3DSExt.stereo.active ? side : GFX_LEFT;

    // Which texture TARGET_SNES_MAIN resolves to depends on the eye, and the eye
    // is not part of the packed render state, so the target has to be forced to
    // re-apply. Without this a frame whose sub screen is empty never changes the
    // target field between the two passes, and the second eye draws into the
    // first eye's screen while its own keeps the frame before.
    GPU3DS.appliedRenderState.target = TARGET_UNSET;

    gpu3dsDrawSnesScreen();

    GPU3DS.snesSide = GFX_LEFT;
    GPU3DS.appliedRenderState.target = TARGET_UNSET;
}

//---------------------------------------------------------
// Holds every slot but one off the screen, so the menu can show
// what a single slot contributes to the frame. -1 draws normally.
//---------------------------------------------------------
void gpu3dsSetDepthSlotIsolation(int slot) {
    // DEPTH3D_SLOT_COUNT itself asks for the backdrop on its own: it carries no
    // slot to isolate by, so every slot is held back and the layer filter lets
    // it through instead.
    GPU3DSExt.stereo.isolateSlot = (s8)(slot >= 0 && slot <= DEPTH3D_SLOT_COUNT ? slot : -1);
}

//---------------------------------------------------------
// Converts the per-game plane depths into a per-layer pixel
// shift for the current 3D slider position. Called once per
// frame so both eyes use the same values.
//---------------------------------------------------------
void gpu3dsUpdateStereoLayerShifts() {
    SStereoLayerState *stereo = &GPU3DSExt.stereo;

    stereo->isolateSlot = -1;

    memset(stereo->slotShift, 0, sizeof(stereo->slotShift));
    memset(stereo->depthSlotSource, -1, sizeof(stereo->depthSlotSource));
    stereo->familiesDrawn = 0;
    stereo->active = false;
    stereo->eye = 0;
    stereo->cropEdges = false;
    stereo->fillGaps = false;

    // The shift is added to vertex positions in target pixels, and the 512px
    // render path draws the frame at twice the scale, so every depth would come
    // out at half the parallax asked for. Per-layer depth is a
    // standard-resolution feature until that path scales the shift to match.
    if (!stereo->supported || !settings3DS.Depth3DEnabled || GPU3DSExt.render2x.enabled || screenshot.dirty)
        return;

    float strength = gpu3dsGetLayerDepthStrength();

    if (strength <= 0.0f)
        return;

    bool anyShift = false;

    for (int family = 0; family < DEPTH3D_FAMILY_COUNT; family++) {
        for (int i = 0; i < DEPTH3D_SLOT_COUNT; i++) {
            int shift = (int)lroundf(settings3DS.Depth3D[family][i] * strength);

            if (shift < -DEPTH3D_SHIFT_MAX) shift = -DEPTH3D_SHIFT_MAX;
            if (shift > DEPTH3D_SHIFT_MAX) shift = DEPTH3D_SHIFT_MAX;

            stereo->slotShift[family][i] = (s8)shift;

            if (shift)
                anyShift = true;
        }
    }

    if (!anyShift)
        return;

    stereo->active = true;
    stereo->cropEdges = settings3DS.Depth3DCropEdges;
    stereo->fillGaps = settings3DS.Depth3DFillGaps;
}

void gpu3dsCommitLayerSection(SGPU_VBO_ID vboId, LAYER_ID id, SGPURenderState *state, bool sub, bool reuseVertices) {
    SLayerList *list = &GPU3DSExt.layerList;
    SLayer *layer = &list->layers[id];

    if (layer->sectionsTotal >= layer->sectionsMax) {
        // skip current frame + count all the skipped sections 
        // to handle layer section limits for the next frame later on (gpu3dsAdjustLayerSectionLimits())
        //
        // This case should rarely happen (and never for LAYER_WINDOW_LR, LAYER_BRIGHTNESS, LAYER_BACKDROP)
        // If at all, it occurs when "In-Frame Pallete Changes" setting is set to "Enabled"
        layer->sectionsSkipped++;
        list->hasSkippedSections = true;
    }   

    int sectionIdx = layer->sectionsOffset + layer->sectionsTotal;

    if (!reuseVertices) 
    {
        SVertexList *vbo = &GPU3DS.vertices[vboId];
        u16 currentIdx = vbo->from;
        u16 currentVerticesCount = gpu3dsGetValueWithinLimit(vbo->count, list->verticesTotal, MAX_VERTICES);

        vbo->from += vbo->count;
        vbo->count = 0;

        // max sections/vertices overflow
        if (list->hasSkippedSections || !currentVerticesCount) return;

        SLayerSection *section = &list->sections[sectionIdx];

        section->state = *state;
        section->from = currentIdx;
        section->count = currentVerticesCount;
        section->vboId = vboId;
        section->onSub = sub;

        if (state->textureBind == SNES_MODE7_TILE_0) {
            layer->m7Tile0 = true;
        }
        
        layer->verticesByTarget[sub] += currentVerticesCount;
        layer->sectionsByTarget[sub]++;
        layer->sectionsTotal++;
        
        list->verticesTotal += currentVerticesCount;
    
        return;
    }

    int prevSectionIndex = sectionIdx - 1;

    if (prevSectionIndex >= 0 && !list->hasSkippedSections)
    {
        SLayerSection *section = &list->sections[sectionIdx]; // reuse last section properties

        *section = list->sections[prevSectionIndex];
        
        if (!section->count) return;

        section->state = *state;
        section->onSub = false; // reuse only happens on main
        
        layer->sectionsByTarget[TARGET_SNES_MAIN]++;
        layer->verticesByTarget[TARGET_SNES_MAIN] += section->count;
        layer->sectionsTotal++;
    }
}

void gpu3dsInitializeMode7Vertex(int idx, s16 x, s16 y)
{
    s16 x0 = 0;
    s16 y0 = 0;

    if (x < 64)
    {
        x0 = x * 8;
        y0 = (y * 2 + 1) * 8;
    }
    else
    {
        x0 = (x - 64) * 8;
        y0 = (y * 2) * 8;
    }

    SMode7TileVertex *m7vertices = &((SMode7TileVertex *)GPU3DS.vertices[VBO_MODE7_TILE].data) [idx];

    m7vertices[0].Position = (SVector4i){x0, y0, 0, -1};
}

void gpu3dsInitializeMode7VertexForTile0(int idx, s16 x, s16 y)
{
    s16 x0 = x;
    s16 y0 = y;

    SMode7TileVertex *m7vertices = &((SMode7TileVertex *)GPU3DS.vertices[VBO_MODE7_TILE].data) [idx];
    
    m7vertices[0].Position = (SVector4i){x0, y0, 0, 0x3fff};
}

void gpu3dsInitializeMode7Vertexes()
{
    GPU3DSExt.mode7FrameCount = 3;
    for (int f = 0; f < 2; f++)
    {
        int idx = 0;
        for (int section = 0; section < 4; section++)
        {
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 128; x++)
                    gpu3dsInitializeMode7Vertex(idx++, x, y);
        }

        gpu3dsInitializeMode7VertexForTile0(16384, 0, 0);
        gpu3dsInitializeMode7VertexForTile0(16385, 0, 8);
        gpu3dsInitializeMode7VertexForTile0(16386, 8, 0);
        gpu3dsInitializeMode7VertexForTile0(16387, 8, 8);

        gpu3dsPrepareListForNextFrame(&GPU3DS.vertices[VBO_MODE7_TILE], true);
    }

	gpu3dsCopyVRAMTilesIntoMode7TileVertexes(Memory.VRAM);
}

// Changes the texture pixel format (but it must be the same 
// size as the original pixel format). No errors will be thrown
// if the format is incorrect.
//

void gpu3dsSetMode7TexturesPixelFormat(GPU_TEXCOLOR fmt)
{
    if (GPU3DSExt.mode7TextureFormat == fmt)
        return;

    GPU3DSExt.mode7TextureFormat = fmt;
    GPU3DS.textures[SNES_MODE7_FULL].tex.fmt = fmt;
    GPU3DS.textures[SNES_MODE7_TILE_0].tex.fmt = fmt;
    GPU3DS.textures[SNES_MODE7_TILE_CACHE].tex.fmt = fmt;

    GPU_COLORBUF colorFmt = (GPU_COLORBUF)gpu3dsGetFrameBufferFmt(fmt);
    GPU3DS.textures[SNES_MODE7_FULL].target->frameBuf.colorFmt = colorFmt;
    GPU3DS.textures[SNES_MODE7_TILE_0].target->frameBuf.colorFmt = colorFmt;
}

void gpu3dsCopyVRAMTilesIntoMode7TileVertexes(uint8 *VRAM)
{
    for (int i = 0; i < 16384; i++)
    {
        gpu3dsSetMode7TileModified(i, VRAM[i * 2]);
    }
    IPPU.Mode7CharDirtyFlagCount = 1;
    for (int i = 0; i < 256; i++)
    {
        IPPU.Mode7CharDirtyFlag[i] = 2;
    }
}

void gpu3dsIncrementMode7UpdateFrameCount()
{
    gpu3dsPrepareListForNextFrame(&GPU3DS.vertices[VBO_MODE7_TILE], true);
    GPU3DSExt.mode7FrameCount ++;

    if (GPU3DSExt.mode7FrameCount == 0x3fff)
    {
        GPU3DSExt.mode7FrameCount = 1;
    }

    // Bug fix: Clears the updateFrameCount of both sets
    // of mode7TileVertexes!
    //
    if (GPU3DSExt.mode7FrameCount <= 2)
    {
        SMode7TileVertex* vertices = (SMode7TileVertex *)GPU3DS.vertices[VBO_MODE7_TILE].data;

        for (int i = 0; i < 16384; )
        {
            vertices[i++].Position.w = -1;
            vertices[i++].Position.w = -1;
            vertices[i++].Position.w = -1;
            vertices[i++].Position.w = -1;

            vertices[i++].Position.w = -1;
            vertices[i++].Position.w = -1;
            vertices[i++].Position.w = -1;
            vertices[i++].Position.w = -1;
        }
    }
}

void gpu3dsAddQuadRect(float x0, float y0, float x1, float y1, u16 wx, u16 wy, int z, u32 fillColor, u32 borderColor, u8 borderSize) 
{
    if (borderSize > 0) {
        float cx0 = x0 + borderSize;
        float cy0 = y0 + borderSize;
        float cx1 = x1 - borderSize;
        float cy1 = y1 - borderSize;
        
        // top, bottom left, right
        gpu3dsAddSimpleQuadVertexes(x0, y0, x1, cy0, wx, wy, wx, wy, z, borderColor);
        gpu3dsAddSimpleQuadVertexes(x0, cy1, x1, y1, wx, wy, wx, wy, z, borderColor);
        gpu3dsAddSimpleQuadVertexes(x0, cy0, cx0, cy1, wx, wy, wx, wy, z, borderColor);
        gpu3dsAddSimpleQuadVertexes(cx1, cy0, x1, cy1, wx, wy, wx, wy, z, borderColor);

        gpu3dsAddSimpleQuadVertexes(cx0, cy0, cx1, cy1, wx, wy, wx, wy, z, fillColor);
    } else {
        gpu3dsAddSimpleQuadVertexes(x0, y0, x1, y1, wx, wy, wx, wy, z, fillColor);
    }
}
