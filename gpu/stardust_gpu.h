/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef STARDUST_GPU_H
#define STARDUST_GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *splash;         /* start-up picture (the dump's sce_sys/pic1.png), NULL: none */
} StardustGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void stardust_gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int stardust_gpu_init(const StardustGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t stardust_gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int stardust_gpu_handle_fault(void *ucontext, void *address);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int stardust_gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int stardust_gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int stardust_gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int stardust_gpu_choice_poll(void);
/* Confirmation with a separate preview of the new save; PNG data is decoded before returning. */
int stardust_gpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                                 const unsigned char *png, uint64_t size, const char *caption, const char *unavailable);
/* Scripted runs (STARDUST_PAD_FILE "settings[=row]"): opens the Display Settings panel on `row`. */
void stardust_gpu_open_display_settings(int row);
/* 1 while a dialog (save list, text entry) is shown: the game's pad input is held neutral. */
int stardust_gpu_overlay_captures_input(void);
/* Opens the Display Settings panel (F10, or Select or Start held for 1 s: runtime_pad.c); 1 while it is open. */
void stardust_display_settings_open(void);
int stardust_display_settings_is_open(void);
/* 1 while a page that pauses the game is open (Display Settings, Cheats): runtime_game_wait_frozen. */
int stardust_overlay_pauses_game(void);
/* Opens the Cheats page (L3+R3: runtime_pad.c), a list of checkboxes. Any thread. */
void stardust_cheats_open(void);
/* L3+R3: the Cheats page opened, or closed when it is open. */
void stardust_cheats_toggle(void);
int stardust_cheats_is_open(void);
/* The cheats switched on, as bits (src/runtime_game.c: CHEAT_*); settings.ini cheats keeps them. */
int stardust_cheats(void);
/* Scripted runs (STARDUST_PAD_FILE "cheats=<bits>"): ticks exactly those cheats. */
void stardust_gpu_set_cheats(int bits);
/* Asks "Exit Super Stardust Ultra VR?" (No focused); Yes closes the game as closing its window does. Any thread. */
void stardust_exit_prompt_open(void);
/* Ends the game as closing its window does (any thread). */
void stardust_gpu_request_quit(void);
/* 1 when settings.ini render_resolution (STARDUST_RENDER_RESOLUTION) is 4k: the game runs in PS4
 * Pro mode with a 3840x2160 video out. Valid once stardust_gpu_init returned. */
int stardust_render_4k(void);
/* settings.ini render_resolution as lines: 720, 1080 (default), 1440 or 2160; the game renders at
 * that height, 16:9 (src/runtime_game.c). Valid once stardust_gpu_init returned. */
int stardust_render_height(void);
/* The game's display width at 2160 lines: 3840 (16:9) for a 16:9 or narrower output, up to 7680
 * (32:9) for a wider one; the output at start decides (the window's size, the desktop's in
 * borderless). The video out is that wide (half with render_resolution=1080p) and
 * runtime_game_widen widens the engine to match. Valid once stardust_gpu_init returned. */
int stardust_width_4k(void);
/* settings.ini antialiasing: the game's temporal anti-aliasing on (1) or off (0); changes live. */
int stardust_antialiasing(void);
/* Sets it back (3840) when the engine could not be widened; before the game opens its video out. */
void stardust_set_width_4k(int width);
/* 0: Super Stardust Ultra VR has no HDR output (it always configures SDR), so the presenter never uses an HDR
 * surface. */
int stardust_hdr(void);
/* 1 when settings.ini skip_intro (STARDUST_SKIP_INTRO, default on) is on: the game starts without
 * its start-up logos and notices (-nosplash). Valid once stardust_gpu_init returned. */
int stardust_skip_intro(void);
/* settings.ini frame_rate (STARDUST_FRAME_RATE): 30, 60, 90, 120, 144 or 180; the Display Settings panel
 * changes it while the game runs. The PS4 vblank runs at it and the engine's target frame rate is set to
 * match at each flip (runtime_game.c). Valid once stardust_gpu_init returned; any thread. */
int stardust_frame_rate(void);
/* Sets frame_rate (one of 30, 60, 90, 120, 144, 180) until the next start; any thread. */
void stardust_set_frame_rate(int rate);
/* sceCommonDialogInitialize ran (runtime_services.c): the upstream dialog libraries check it. */
void stardust_gpu_common_dialog_initialized(void);
/* Speaker layout for the game's 7.1 audio (settings.ini audio_channels): 0 automatic (the
 * device's), 1 stereo, 2 5.1, 3 7.1. Read when an audio port is opened. */
int stardust_gpu_audio_channels(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned stardust_gpu_symbol_count(void);
/* Yields of threads waiting for guest memory copies (CPU report). */
unsigned long long stardust_gpu_copy_yields(void);
/* The hang dump's GPU lines: how far the GPU got on each timeline it is waited on. */
void stardust_gpu_hang_report(void);
/* Frames handed to the presenter since start (hang dump). */
unsigned long long stardust_gpu_flips(void);
/* A notice in the corner for `milliseconds` (any thread; the runtime's messages to the player). */
void stardust_gpu_notice(const char *text, int milliseconds);
/* Trophy unlocked (any thread): its name as a notice in the corner. The other arguments are kept
 * for the runtime's call: id, description and PNG icon. */
void stardust_gpu_toast_trophy(int id, const char *title, const char *detail, const unsigned char *png,
                                 uint64_t png_size);
#ifdef __cplusplus
}
#endif
#endif
