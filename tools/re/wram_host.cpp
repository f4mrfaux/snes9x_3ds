/* Headless libretro host that records how a game's work RAM changes, for finding
 * the variable a game keeps its current scene in. See rank_scene_vars.py.
 *
 *   wram_host <core.dll> <rom> <outdir> <frames> [hold=<frame>:<button>[:<frames>]...]
 *             [movie=<file>[:<offset>]] [shot=<frame>...]
 *
 * movie= plays back port 1 input: u16 little endian per frame, in SMV button layout.
 * offset is added to the frame number when looking up input, to line a movie up.
 *
 * Writes to outdir, which must stay out of the repository since it holds game RAM:
 *   wram.delta  the first frame's RAM, then each frame's changed bytes
 *   frames.csv  frame, mean luma (0-255), share of sampled pixels changed (0-1)
 *   ppu.csv     per-frame PPU state, if the core has core-ppu-export.patch applied
 *   shot_<frame>.ppm  the picture at each shot= frame
 *
 * wram.delta layout, little endian:
 *   u32 ram_size, u8 ram[ram_size]
 *   per frame: u32 frame, u32 count, u32 change[count] (address | value << 24)
 */
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "libretro.h"

typedef void (*set_env_t)(retro_environment_t);
typedef void (*set_video_t)(retro_video_refresh_t);
typedef void (*set_audio_t)(retro_audio_sample_t);
typedef void (*set_audio_batch_t)(retro_audio_sample_batch_t);
typedef void (*set_poll_t)(retro_input_poll_t);
typedef void (*set_state_t)(retro_input_state_t);
typedef void (*void_fn_t)(void);
typedef bool (*load_game_t)(const struct retro_game_info *);
typedef void *(*get_mem_data_t)(unsigned);
typedef size_t (*get_mem_size_t)(unsigned);
typedef size_t (*ppu_state_t)(uint8_t *, size_t);

struct hold { unsigned frame, button, length; };

static std::vector<hold> g_holds;
static std::vector<uint16_t> g_movie;
static std::vector<unsigned> g_shots;
static std::string g_outdir;
static int g_movie_offset;
static unsigned g_frame;
static FILE *g_csv;

/* SMV bit -> libretro joypad id, -1 for the reserved low bits. */
static const int8_t g_smv_buttons[16] = {
   -1, -1, -1, -1,
   RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_A,
   RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_UP,
   RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_B
};

static void write_shot(const void *data, unsigned w, unsigned h, size_t pitch)
{
   FILE *f = fopen((g_outdir + "/shot_" + std::to_string(g_frame) + ".ppm").c_str(), "wb");
   if (!f) return;
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   for (unsigned y = 0; y < h; y++)
   {
      const uint16_t *row = (const uint16_t *)((const uint8_t *)data + y * pitch);
      for (unsigned x = 0; x < w; x++)
      {
         uint8_t rgb[3] = { (uint8_t)(((row[x] >> 11) & 31) * 255 / 31),
                            (uint8_t)(((row[x] >> 5) & 63) * 255 / 63),
                            (uint8_t)((row[x] & 31) * 255 / 31) };
         fwrite(rgb, 1, 3, f);
      }
   }
   fclose(f);
}

/* A coarse grid is enough to tell a scene cut from ordinary motion. */
enum { GRID_W = 64, GRID_H = 56 };
static uint16_t g_prev_grid[GRID_W * GRID_H];
static bool g_have_prev;

static void RETRO_CALLCONV video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   uint16_t grid[GRID_W * GRID_H];
   uint64_t luma = 0;
   unsigned changed = 0;

   if (!data || !w || !h) return;

   for (size_t i = 0; i < g_shots.size(); i++)
      if (g_shots[i] == g_frame) write_shot(data, w, h, pitch);

   for (unsigned y = 0; y < GRID_H; y++)
   {
      const uint16_t *row = (const uint16_t *)((const uint8_t *)data + (y * h / GRID_H) * pitch);
      for (unsigned x = 0; x < GRID_W; x++)
      {
         uint16_t p = row[x * w / GRID_W];
         grid[y * GRID_W + x] = p;
         /* RGB565 to 0-255 luma, near enough to find a fade. */
         luma += (((p >> 11) & 31) * 255 / 31 * 2 + ((p >> 5) & 63) * 255 / 63 * 5 + (p & 31) * 255 / 31) / 8;
         if (g_have_prev && p != g_prev_grid[y * GRID_W + x]) changed++;
      }
   }

   memcpy(g_prev_grid, grid, sizeof(grid));
   fprintf(g_csv, "%u,%.2f,%.4f\n", g_frame, (double)luma / (GRID_W * GRID_H),
         g_have_prev ? (double)changed / (GRID_W * GRID_H) : 0.0);
   g_have_prev = true;
}

static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r) {}
static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames) { return frames; }
static void RETRO_CALLCONV poll_cb(void) {}

static int16_t RETRO_CALLCONV state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
   int16_t mask = 0;
   if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0) return 0;
   for (size_t i = 0; i < g_holds.size(); i++)
      if (g_frame >= g_holds[i].frame && g_frame < g_holds[i].frame + g_holds[i].length)
         mask |= (int16_t)(1 << g_holds[i].button);
   int64_t at = (int64_t)g_frame + g_movie_offset;
   if (at >= 0 && at < (int64_t)g_movie.size())
      for (int b = 0; b < 16; b++)
         if (g_smv_buttons[b] >= 0 && (g_movie[(size_t)at] >> b & 1))
            mask |= (int16_t)(1 << g_smv_buttons[b]);
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return mask;
   return (mask >> id) & 1;
}

static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data)
{
   static const char *dir = ".";
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = dir;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         /* Movies can hold left+right, which the core filters out by default. */
         struct retro_variable *var = (struct retro_variable *)data;
         if (!g_movie.empty() && !strcmp(var->key, "snes9x_up_down_allowed"))
         {
            var->value = "enabled";
            return true;
         }
         return false;
      }
      default:
         return false;
   }
}

int main(int argc, char **argv)
{
   if (argc < 5)
   {
      fprintf(stderr, "usage: wram_host core rom outdir frames [hold=frame:button[:frames]...]\n");
      return 2;
   }

   HMODULE core = LoadLibraryA(argv[1]);
   if (!core) { fprintf(stderr, "load core failed %lu\n", GetLastError()); return 1; }

   std::string outdir = argv[3];
   g_outdir = outdir;
   unsigned frames = (unsigned)atoi(argv[4]);

   for (int i = 5; i < argc; i++)
   {
      if (!strncmp(argv[i], "shot=", 5)) { g_shots.push_back((unsigned)atoi(argv[i] + 5)); continue; }
      if (!strncmp(argv[i], "movie=", 6))
      {
         std::string path = argv[i] + 6;
         size_t colon = path.rfind(':');
         if (colon != std::string::npos && colon > 1)
         {
            g_movie_offset = atoi(path.c_str() + colon + 1);
            path.resize(colon);
         }
         FILE *m = fopen(path.c_str(), "rb");
         if (!m) { fprintf(stderr, "open movie failed\n"); return 1; }
         fseek(m, 0, SEEK_END); g_movie.resize((size_t)ftell(m) / 2); fseek(m, 0, SEEK_SET);
         if (!g_movie.empty()) fread(&g_movie[0], 2, g_movie.size(), m);
         fclose(m);
         continue;
      }
      if (strncmp(argv[i], "hold=", 5)) { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
      hold h = { 0, 0, 4 };
      if (sscanf(argv[i] + 5, "%u:%u:%u", &h.frame, &h.button, &h.length) < 2)
      {
         fprintf(stderr, "bad hold %s\n", argv[i]);
         return 2;
      }
      g_holds.push_back(h);
   }

   FILE *f = fopen(argv[2], "rb");
   if (!f) { fprintf(stderr, "open rom failed\n"); return 1; }
   std::vector<uint8_t> rom;
   fseek(f, 0, SEEK_END); rom.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
   fread(&rom[0], 1, rom.size(), f); fclose(f);

   g_csv = fopen((outdir + "/frames.csv").c_str(), "w");
   FILE *delta = fopen((outdir + "/wram.delta").c_str(), "wb");
   if (!g_csv || !delta) { fprintf(stderr, "open output failed\n"); return 1; }
   fprintf(g_csv, "frame,luma,changed\n");

#define SYM(type, name) ((type)GetProcAddress(core, name))
   SYM(set_env_t, "retro_set_environment")(env_cb);
   SYM(set_video_t, "retro_set_video_refresh")(video_cb);
   SYM(set_audio_t, "retro_set_audio_sample")(audio_cb);
   SYM(set_audio_batch_t, "retro_set_audio_sample_batch")(audio_batch_cb);
   SYM(set_poll_t, "retro_set_input_poll")(poll_cb);
   SYM(set_state_t, "retro_set_input_state")(state_cb);
   SYM(void_fn_t, "retro_init")();

   struct retro_system_info sys;
   memset(&sys, 0, sizeof(sys));
   ((void (*)(struct retro_system_info *))GetProcAddress(core, "retro_get_system_info"))(&sys);

   struct retro_game_info game;
   memset(&game, 0, sizeof(game));
   game.path = argv[2];
   if (!sys.need_fullpath) { game.data = &rom[0]; game.size = rom.size(); }
   if (!SYM(load_game_t, "retro_load_game")(&game)) { fprintf(stderr, "load game failed\n"); return 1; }

   void_fn_t run = SYM(void_fn_t, "retro_run");
   const uint8_t *ram = (const uint8_t *)SYM(get_mem_data_t, "retro_get_memory_data")(RETRO_MEMORY_SYSTEM_RAM);
   uint32_t ram_size = (uint32_t)SYM(get_mem_size_t, "retro_get_memory_size")(RETRO_MEMORY_SYSTEM_RAM);
   if (!ram || !ram_size) { fprintf(stderr, "core exposes no system RAM\n"); return 1; }

   /* Optional: a stock core doesn't have this, and the rest still works. */
   ppu_state_t ppu_state = SYM(ppu_state_t, "snes9x_re_ppu_state");
   FILE *ppu_csv = NULL;
   uint8_t ppu[35];
   if (ppu_state)
   {
      if (ppu_state(NULL, 0) != sizeof(ppu)) { fprintf(stderr, "PPU export layout mismatch\n"); return 1; }
      ppu_csv = fopen((outdir + "/ppu.csv").c_str(), "w");
      if (!ppu_csv) { fprintf(stderr, "open ppu.csv failed\n"); return 1; }
      fprintf(ppu_csv, "frame,mode,bg3prio,bright,blank,tm,ts,cgwsel,cgadsub,"
            "sc0,name0,size0,sc1,name1,size1,sc2,name2,size2,sc3,name3,size3,"
            "objname,objsize,obj0,obj1,obj2,obj3\n");
   }

   std::vector<uint8_t> prev(ram, ram + ram_size);
   std::vector<uint32_t> changes;
   uint64_t total = 0;

   fwrite(&ram_size, 4, 1, delta);
   fwrite(ram, 1, ram_size, delta);

   for (g_frame = 0; g_frame < frames; g_frame++)
   {
      run();

      if (ppu_csv)
      {
         ppu_state(ppu, sizeof(ppu));
         fprintf(ppu_csv, "%u,%u,%u,%u,%u,%u,%u,%u,%u", g_frame, ppu[0], ppu[1], ppu[2], ppu[3],
               ppu[4], ppu[5], ppu[6], ppu[7]);
         for (int i = 0; i < 4; i++)
         {
            const uint8_t *bg = ppu + 8 + i * 5;
            fprintf(ppu_csv, ",%u,%u,%u", bg[0] | bg[1] << 8, bg[2] | bg[3] << 8, bg[4]);
         }
         fprintf(ppu_csv, ",%u,%u,%u,%u,%u,%u\n", ppu[28] | ppu[29] << 8, ppu[30],
               ppu[31], ppu[32], ppu[33], ppu[34]);
      }

      changes.clear();
      for (uint32_t a = 0; a < ram_size; a++)
      {
         if (ram[a] != prev[a])
         {
            changes.push_back(a | ((uint32_t)ram[a] << 24));
            prev[a] = ram[a];
         }
      }

      uint32_t head[2] = { g_frame, (uint32_t)changes.size() };
      fwrite(head, 4, 2, delta);
      if (!changes.empty()) fwrite(&changes[0], 4, changes.size(), delta);
      total += changes.size();
   }

   /* FNV-1a of the final RAM, so a replay of wram.delta can be checked against it. */
   uint64_t hash = 14695981039346656037ULL;
   for (uint32_t a = 0; a < ram_size; a++) { hash ^= ram[a]; hash *= 1099511628211ULL; }

   printf("frames=%u ram=%u changes=%llu ram_fnv=%016llx\n", frames, ram_size,
         (unsigned long long)total, (unsigned long long)hash);

   SYM(void_fn_t, "retro_unload_game")();
   SYM(void_fn_t, "retro_deinit")();
   fclose(delta);
   if (ppu_csv) fclose(ppu_csv);
   fclose(g_csv);
   return 0;
}
