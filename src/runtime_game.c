/* The game's version, logged first so a bug report's log tells which dump it came from: param.sfo's
 * TITLE_ID and APP_VER, and the eboot's SHA-256 that scripts/prepare.py records in analysis.json
 * beside the image. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include "gpu/stardust_gpu.h"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* eboot.bin builds the runner has been run with, and in each: the global holding the engine's frame
 * clock, set_fps (the clock's vtable slot 3), and the read-only data holding that vtable. */
typedef struct {
    const char *sha256, *version;
    uint32_t clock, set_fps, data, data_end;
    uint32_t frame_loop;              /* global: the main loop object + 0x40 */
    uint32_t hmd_sleep[2], sleep_plt; /* sceKernelSleep calls of the PS VR headset wait, its PLT stub */
    uint32_t size_movabs[2];          /* movabs rax, 1080<<32|1920: the render device's and the screen's sizes */
    uint32_t size_movl[2];            /* movl $1080 / movl $1920 to the stack: the view's size */
    uint32_t particle_start_slot, particle_start, streak_mul; /* particle task start: its vtable slot and target; the streak multiply */
    uint32_t scene_step[2];           /* vmovss xmm0,[1/59.94]: the renderer's per-frame scene step */
    /* Cheats: Player_Master_Impl's update (vtable slot 2) and its target, the game session lookup
     * by id, the sd3__game__god_mode value, the maximum_weaponN_power values, the respawn's
     * `lea ecx,[rax-1]` and its `jg` past the last-ship notice, the smart bomb's `dec ecx`. */
    uint32_t master_update_slot, master_update, session_by_id, god_mode, weapon_max[3];
    uint32_t life_spend, life_last, bomb_spend;
} Build;
static const Build known_builds[]={
    {"4a89c3e1c9663de0a411a446207348c5689a165a7a9afec958e79dfe674ebbc9","01.00",
     0x9b6250,0x53fe40,0x6e8000,0x714000,0xcdf378,{0x556e5c,0x556ea2},0x56b810,
     {0x41ee1b,0x556899},{0x2623ca,0x2623d5},0x700a90,0x489600,0x489349,{0x422e1a,0x422e80},
     0x707438,0x4d1fa0,0x4b03e0,0x9be37c,{0x9be3bc,0x9be3fc,0x9be43c},0x4d386d,0x4d387a,0x4d5cc4},
};
static const Build *build;
static unsigned char *game_image;
static _Atomic float rate_scale=1.0f; /* rate/59.94, set by apply_frame_rate: particle cadence (patch_particles) */
static float *scene_step; /* the renderer's per-frame scene step, 1/rate (patch_scene_step) */

/* "source_sha256" from analysis.json in the folder of `image_path`. */
static int eboot_sha256(const char *image_path, char *out) {
    char path[4096];
    const char *slash=strrchr(image_path,'/'), *back=strrchr(image_path,'\\');
    if (back>slash) slash=back;
    const int dir=slash ? (int)(slash-image_path)+1 : 0;
    snprintf(path,sizeof(path),"%.*sanalysis.json",dir,image_path);
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    char head[4096];
    const size_t n=fread(head,1,sizeof(head)-1,f);
    fclose(f);
    head[n]=0;
    const char *sha=strstr(head,"\"source_sha256\": \"");
    if (!sha || strspn(sha+18,"0123456789abcdef")<64) return 0;
    memcpy(out,sha+18,64); out[64]=0;
    return 1;
}

void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path) {
    (void)app0;
    char sha[65]="";
    if (eboot_sha256(image_path,sha))
        for (size_t i=0;i<sizeof(known_builds)/sizeof(*known_builds);++i)
            if (!strcmp(sha,known_builds[i].sha256)) build=&known_builds[i];
    printf("Game: %s version %s, eboot.bin sha256 %s (%s%s)\n",title_id[0] ? title_id : "?",
           app_version[0] ? app_version : "unknown",sha[0] ? sha : "unknown",
           build ? "known build " : "a build the runner was not checked with",build ? build->version : "");
}

/* STARDUST_POKE=<offset>=<hex bytes>[,...] (diagnosis): writes bytes into the image's writable data
 * before the game's entry, e.g. 0x71429c=01 turns on the engine's log channel 0. */
static void poke(unsigned char *image) {
    const char *e=getenv("STARDUST_POKE");
    while (e && *e) {
        char *end;
        const unsigned long long at=strtoull(e,&end,16);
        if (*end!='=') break;
        e=end+1;
        size_t n=0;
        while (strspn(e,"0123456789abcdefABCDEF")>=2) {
            char byte[3]={e[0],e[1],0};
            image[at+n++]=(unsigned char)strtoul(byte,NULL,16);
            e+=2;
        }
        printf("Poke: %zu byte(s) at image +%#llx\n",n,at);
        if (*e!=',') break;
        ++e;
    }
}

void runtime_game_set_image(unsigned char *image) { game_image=image; poke(image); }

/* Much of the game's logic was written for one update every 1/59.94 s and ignores the step it is
 * given: `x += (target-x)*k`, `v *= m` and frame counts run once per update, so above 60 FPS the
 * menus move faster, the beam weapon bends tighter, the ship boosts and enemies turn quicker. Those
 * constants are the engine's named tunables (sd3__..., registered at static init; the value is at
 * +0xc of each), read where they are used, so each one used per update is rescaled to the update
 * rate, r = 59.94/rate updates of the new length per old one: a lerp k becomes 1-(1-k)^r, a
 * multiplier m becomes m^r, a per-update amount s becomes s*r and a frame count n becomes n/r; a
 * countdown that fires on the update after it reaches 0 (n+1 updates) becomes (n+1)/r-1.
 * The game's own value is kept and taken again whenever the game writes a new one. */
enum { LERP, MUL, PER_UPDATE, FRAMES_INT, FRAMES_FLOAT, COUNTDOWN_INT, COUNTDOWN_FLOAT };
typedef struct { uint32_t value; uint8_t kind; } Tunable;
static const Tunable tunables_0100[]={
    /* bullets: the beam's segments follow the one before (shorter, tighter beams); its audio */
    {0x9b6d14,LERP},{0x9b6df4,LERP},{0x9b6e14,LERP},{0x9b6cd4,LERP},{0x9b6c74,LERP},{0x9b6ed4,LERP},
    /* camera */
    {0x9cc58c,LERP},{0x9cc5ac,LERP},{0x9cc5cc,LERP},{0x9cc5ec,LERP},{0x9cc6cc,LERP},
    /* player ship: boost speed up/down, turning by boost time left, thruster sound, motion sensor */
    {0x9bed9c,LERP},{0x9bedbc,LERP},{0x9beddc,LERP},{0x9bedfc,LERP},
    {0x9be7a4,LERP},{0x9be7c4,LERP},{0x9be824,LERP},{0x9be844,LERP},{0x9beab4,LERP},{0x9bdcf4,LERP},
    /* enemies: bosses, missiles, cannon, centipedes, lurker, spinner, swarmer, weapon ship */
    {0x9b79c4,LERP},{0x9b7be4,LERP},{0x9b79a4,LERP},{0x9b7fc4,LERP},{0x9b7e24,LERP},{0x9b8384,LERP},
    {0x9b88a4,LERP},{0x9b8984,LERP},{0x9b8c84,LERP},
    {0x9b8da4,LERP},{0x9b9144,LERP},{0x9b91a4,LERP},{0x9b8fc4,LERP},{0x9b91c4,MUL},
    {0x9b92a4,LERP},{0x9b94a4,LERP},{0x9b9504,LERP},{0x9b9464,LERP},{0x9b9524,MUL},
    {0x9b97c4,LERP},{0x9b96c4,LERP},{0x9b9704,LERP},{0x9b97a4,LERP},
    {0x9ba724,LERP},{0x9ba804,LERP},{0x9ba7e4,LERP},{0x9ba624,LERP},{0x9ba6e4,LERP},{0x9ba704,LERP},
    {0x9bab44,LERP},{0x9baba4,LERP},{0x9bab84,LERP},{0x9bada4,LERP},
    /* HUD meters, multiplier countdown, markers, screen fades, phase-done slow-down, grid, tokens */
    {0x9be27c,LERP},{0x9b4f6c,LERP},{0x9bdf0c,FRAMES_INT},{0x9bd5ec,LERP},{0x9cc39c,LERP},{0x9cc3bc,LERP},
    {0x9bbf34,LERP},{0x9bbf54,LERP},{0x9bb74c,LERP},{0x9bb72c,LERP},{0x9bb76c,LERP},
    {0x9ccc7c,LERP},{0x9ccbbc,MUL},{0x9ccbdc,MUL},{0x9cc2dc,MUL},{0x9cc2fc,MUL},
    /* versus: markers, motion sensor, thruster, turning, tokens, rumble, frictions, death delay */
    {0x9d9014,LERP},{0x9d91e4,LERP},{0x9dc4f4,LERP},{0x9dc314,LERP},{0x9ddadc,LERP},
    {0x9ddbdc,MUL},{0x9ddbbc,MUL},{0x9dd30c,MUL},{0x9dd32c,MUL},
    {0x9d6d94,MUL},{0x9d6c94,MUL},{0x9dc3d4,MUL},{0x9dc904,MUL},{0x9dbb8c,FRAMES_FLOAT},
    /* menus: page movement and button response (the *_decay ones keep that share of the distance
     * left: x = target + (x-target)*k, so MUL), fades, loading object, ship editor, press start logo
     * (decays too, 0x4fd16a), background fireworks, terms scrolling, cursor blink; held-direction key
     * repeat (0x36e2b0: the first repeat after delay_cycles updates, later ones after
     * repeat_count_to_cycles' "to" range) */
    {0x9cf0ac,MUL},{0x9cf0cc,MUL},{0x9cf0ec,MUL},{0x9ccf5c,LERP},{0x9cd1ec,LERP},{0x9cee1c,LERP},
    {0x9ce3b4,LERP},{0x9ce8ac,MUL},{0x9ce8cc,MUL},{0x9ce90c,MUL},{0x9cd7ec,MUL},
    {0x9cec1c,PER_UPDATE},{0x9cec3c,PER_UPDATE},{0x9cf06c,FRAMES_INT},
    {0x9cd23c,COUNTDOWN_INT},{0x9cd29c,COUNTDOWN_FLOAT},{0x9cd2bc,COUNTDOWN_FLOAT},
};
#define TUNABLES (sizeof(tunables_0100)/sizeof(*tunables_0100))
static uint32_t tunable_game[TUNABLES], tunable_ours[TUNABLES]; /* bit patterns */
static float tunable_r=1.0f;
static uint32_t rescale(uint32_t bits, int kind, float r) {
    float f; int32_t n; memcpy(&f,&bits,4); memcpy(&n,&bits,4);
    switch (kind) {
    case LERP: if (f>0.0f && f<1.0f) f=1.0f-powf(1.0f-f,r); break;
    case MUL: if (f>0.0f) f=powf(f,r); break;
    case PER_UPDATE: f*=r; break;
    case FRAMES_FLOAT: f/=r; break;
    case FRAMES_INT: if (n>0) { n=(int32_t)lroundf((float)n/r); if (n<1) n=1; } memcpy(&bits,&n,4); return bits;
    case COUNTDOWN_FLOAT: if (f>=0.0f) f=(f+1.0f)/r-0.5f; break; /* the game truncates it: +0.5 rounds */
    case COUNTDOWN_INT: if (n>=0) n=(int32_t)lroundf((float)(n+1)/r)-1; memcpy(&bits,&n,4); return bits;
    }
    memcpy(&bits,&f,4);
    return bits;
}
static void rescale_tunables(float rate) {
    if (!build || strcmp(build->version,"01.00")) return;
    const float r=59.94f/rate;
    const int changed=r!=tunable_r;
    if (changed) printf("Frame rate: %zu per-update constants rescaled for %.2f updates a second\n",TUNABLES,(double)rate);
    tunable_r=r;
    for (size_t i=0;i<TUNABLES;++i) {
        uint32_t *v=(uint32_t *)(game_image+tunables_0100[i].value);
        if (*v!=tunable_ours[i]) tunable_game[i]=*v; /* the game set it (start, or a script) */
        else if (!changed) continue;
        tunable_ours[i]=r==1.0f ? tunable_game[i] : rescale(tunable_game[i],tunables_0100[i].kind,r);
        *v=tunable_ours[i];
    }
}

/* Multipliers the game applies once per update that are plain constants, not tunables: each is a
 * vmovss xmmN,[rip+...] load of a float only that load reads. The load is pointed at a float in the
 * patch data page (from +8), which apply_frame_rate keeps at m^(59.94/rate). */
typedef struct { uint32_t load; float value; } UpdateConstant;
static const UpdateConstant update_constants_0100[]={
    /* player ship velocity damping (ship update 0x31b6b0): the boost's dash went shorter */
    {0x31cd76,0.95f},{0x31cdbd,0.9f},
};
#define UPDATE_CONSTANTS (sizeof(update_constants_0100)/sizeof(*update_constants_0100))
static float *update_constant[UPDATE_CONSTANTS];
static void patch_update_constants(unsigned char *image, unsigned char *patch_data) {
    size_t patched=0;
    for (size_t i=0;i<UPDATE_CONSTANTS;++i) {
        unsigned char *at=image+update_constants_0100[i].load;
        int32_t rel; memcpy(&rel,at+4,4);
        float value; memcpy(&value,at+8+rel,4);
        if (at[0]!=0xc5 || at[1]!=0xfa || at[2]!=0x10 || (at[3]&0xc7)!=0x05 || value!=update_constants_0100[i].value) continue;
        float *slot=(float *)(patch_data+8+4*i);
        *slot=value;
        rel=(int32_t)((intptr_t)slot-(intptr_t)(at+8));
        memcpy(at+4,&rel,4);
        update_constant[i]=slot;
        ++patched;
    }
    printf("Frame rate: %zu of %zu per-update constants follow the frame rate\n",patched,(size_t)UPDATE_CONSTANTS);
}
static void rescale_update_constants(float rate) {
    const float r=59.94f/rate;
    for (size_t i=0;i<UPDATE_CONSTANTS;++i)
        if (update_constant[i]) *update_constant[i]=r>0.999f && r<1.001f ? update_constants_0100[i].value : powf(update_constants_0100[i].value,r);
}

/* Menu page transitions: UI_Page's update (01.00 0x3925e0) fades each page toward shown or hidden
 * with alpha = target + (alpha-target)*k once per update, k a field (+0x104) both constructors set
 * to 0.7, and the menu drops the outgoing page once its alpha is under 0.01: a page change took 4
 * updates whatever their length. The multiply (vmulss xmm2,xmm2,[rdi+0x104] at 0x392628) reads a
 * float in the patch data page (+0x40) instead, which apply_frame_rate keeps at 0.7^(59.94/rate). */
#define PAGE_FADE_MUL 0x392628
#define PAGE_FADE_K 0.7f
_Static_assert(8+4*UPDATE_CONSTANTS<=0x40,"update constants run into the page fade's patch data");
static float *page_fade;
static void patch_page_fade(unsigned char *image, unsigned char *patch_data) {
    unsigned char *at=image+PAGE_FADE_MUL;
    static const unsigned char want[8]={0xc5,0xea,0x59,0x97,0x04,0x01,0x00,0x00};
    if (memcmp(at,want,8)) { puts("Menus: the page fade is not where eboot 01.00 has it; page changes run faster above 60 FPS"); return; }
    page_fade=(float *)(patch_data+0x40);
    *page_fade=PAGE_FADE_K;
    const int32_t rel=(int32_t)((intptr_t)page_fade-(intptr_t)(at+8));
    at[3]=0x15; /* xmm2, [rip+rel32] */
    memcpy(at+4,&rel,4);
    puts("Menus: page fades follow the frame rate");
}
static void rescale_page_fade(float rate) {
    const float r=59.94f/rate;
    if (page_fade) *page_fade=r>0.999f && r<1.001f ? PAGE_FADE_K : powf(PAGE_FADE_K,r);
}

/* settings.ini frame_rate onto the engine's frame clock. The main loop (01.00 0x426e10) measures
 * the real time since the last frame, runs the game's update in fixed steps of the clock's 1/fps
 * (at +0xc), each given that step as its delta time, and waits out the rest of the display
 * interval (+0x14): the game sets 59.94 at boot, so it ran at 60 whatever the vblank. Its speed is
 * real time at any rate. The clock is a global object (01.00 +0x9b6250); set_fps(float) is vtable
 * slot 3 (target at +8, step at +0xc; it also sets the display rate at +0x10/+0x14 through slot 5).
 * Applied at each flip, which also restores the rate after the game resets it. Before the first
 * change the clock is checked: a vtable in the image's read-only data, set_fps where this build has
 * it and a target near 60 (until the game sets one, the target is FLT_MIN). */
static int clock_state; /* 0 unchecked, 1 good, -1 not the clock this build has */
static void apply_frame_rate(void) {
    if (!game_image || clock_state<0) return;
    if (!build) {
        clock_state=-1;
        stardust_set_frame_rate(60);
        puts("Frame rate: an eboot the runner does not know; the game keeps its own 60 FPS");
        return;
    }
    float *clock=*(float **)(game_image+build->clock);
    if (!clock) return;
    void **vtable=*(void ***)clock;
    const float target=clock[2];
    if (!clock_state) {
        if (target<1.0f) return; /* not set yet */
        const uintptr_t v=(uintptr_t)vtable-(uintptr_t)game_image, set=(uintptr_t)vtable[3]-(uintptr_t)game_image;
        if (v<build->data || v>=build->data_end || set!=build->set_fps || !(target>59.0f && target<60.5f)) {
            clock_state=-1;
            stardust_set_frame_rate(60);
            printf("Frame rate: the engine's frame clock is not where eboot %s has it (vtable +%#zx, "
                   "target %.2f); the game keeps its own 60 FPS\n",build->version,(size_t)v,(double)target);
            return;
        }
        clock_state=1;
        printf("Frame rate: engine clock found (game's target %.2f FPS)\n",(double)target);
    }
    const int rate=stardust_frame_rate();
    /* 60 is the game's own 59.94 */
    if (!(target==(float)rate || (rate==60 && target>59.0f && target<60.5f))) {
        typedef void (ABI *SetFps)(void *clock, float fps);
        ((SetFps)vtable[3])(clock,rate==60 ? 59.94f : (float)rate);
        printf("Frame rate: %d FPS (settings.ini frame_rate)\n",rate);
    }
    atomic_store(&rate_scale,clock[2]/59.94f);
    if (scene_step) *scene_step=1.0f/clock[2];
    rescale_tunables(clock[2]);
    rescale_update_constants(clock[2]);
    rescale_page_fade(clock[2]);
}

/* settings.ini skip_intro. The start-up screens (the Sony, XDEV and Housemarque logos, D3T's logo
 * movie and the software usage terms) run on timers until the terms wait for Cross; the title
 * screen comes with the menu music (menu.mp3, or its orchestral_ or retro_ soundtrack). Until it
 * opens, the main loop is fast-forwarded: its base time (01.00: main loop object +0x18, then +0x18,
 * nanoseconds) is moved back 50 ms at each flip, so it runs its most updates a frame (4) and the
 * timers go 4 times as fast; runtime_pad.c taps Cross on the terms, and AvPlayer ends the movie
 * after its first frame. */
static _Atomic int intro_over;
int runtime_game_in_intro(void) { return stardust_skip_intro() && !atomic_load(&intro_over); }
void runtime_game_opened(const char *path) {
    const size_t n=strlen(path);
    if (n<8 || strcmp(path+n-8,"menu.mp3") || atomic_exchange(&intro_over,1)) return;
    if (stardust_skip_intro()) puts("Intro: menu music; the start-up screens are over");
}
static void fast_forward_intro(void) {
    if (!build || !runtime_game_in_intro()) return;
    const uint64_t loop=*(uint64_t *)(game_image+build->frame_loop);
    if (!loop) return;
    const uint64_t clock=*(uint64_t *)(uintptr_t)(loop-0x40+0x18);
    if (clock) *(int64_t *)(uintptr_t)(clock+0x18)-=50000000;
}

/* The Display Settings and Cheats pages stop the game while open: the game's thread waits at its
 * next flip (the presenter redraws the last frame under the panel, MustKeepDrawing) and audio
 * output waits too, so music and sounds pause. The main loop's accumulator drops real time
 * beyond about four steps, so the game does not race to catch up when the panel closes. */
int runtime_game_frozen(void) { return stardust_overlay_pauses_game(); }
void runtime_game_wait_frozen(void) {
    if (!runtime_game_frozen()) return;
    while (runtime_game_frozen()) SDL_Delay(5);
}

/* At each flip, on the game's thread. */
void runtime_game_frame_rate(void) {
    if (runtime_game_frozen()) {
        puts("Runtime: game frozen while an overlay page is open");
        runtime_game_wait_frozen();
        puts("Runtime: game resumed");
    }
    apply_frame_rate(); fast_forward_intro();
}

/* The game waits for a PS VR headset at start: sceKernelSleep(2), then up to three
 * sceKernelSleep(1) between polls. A PC has none, so the sleeps go (5 s off every start); the
 * polls stay, and the game goes on without the headset as before. Each call is checked to be
 * `call sceKernelSleep` before it becomes a 5-byte nop. */
/* settings.ini render_resolution. The engine sizes everything it draws from 1920x1080 written in
 * three places: the render device's description (its scene targets, the bloom chain at halves of
 * it, the display buffers it registers), the screen's (the 2D layout) and the view's. Each gets
 * the chosen 16:9 size, after its instruction is checked to hold 1920 and 1080; the game then
 * draws natively at that size, HUD and menus included. */
static void patch_render_size(unsigned char *image) {
    const uint32_t h=(uint32_t)stardust_render_height(), w=h*16/9;
    if (h==1080) return;
    static const unsigned char movabs[10]={0x48,0xb8,0x80,0x07,0x00,0x00,0x38,0x04,0x00,0x00};
    static const unsigned char movl_h[4]={0x38,0x04,0x00,0x00}, movl_w[4]={0x80,0x07,0x00,0x00};
    for (int i=0;i<2;++i)
        if (memcmp(image+build->size_movabs[i],movabs,10)) { printf("Render: the render size is not where eboot %s has it; the game draws at 1080p\n",build->version); return; }
    if (memcmp(image+build->size_movl[0]+7,movl_h,4) || memcmp(image+build->size_movl[1]+7,movl_w,4)) {
        printf("Render: the view size is not where eboot %s has it; the game draws at 1080p\n",build->version);
        return;
    }
    const uint64_t size=(uint64_t)h<<32|w;
    for (int i=0;i<2;++i) memcpy(image+build->size_movabs[i]+2,&size,8);
    memcpy(image+build->size_movl[0]+7,&h,4);
    memcpy(image+build->size_movl[1]+7,&w,4);
    printf("Render: the game draws at %ux%u (settings.ini render_resolution)\n",w,h);
}

/* Particles (the plasma shots and their trails, sparks, debris, explosions) are simulated by a
 * particle task started once per update with that update's step (Particle_Simulation_Task_Impl
 * vtable slot 2, 01.00 0x489600), which also builds the particles' vertices for that frame (a task
 * not run draws no particles). Its effects were authored for 59.94 runs a second: run more often,
 * with the same total time, the shots' trails come out shorter and some effects change shape. So
 * the start is hooked to simulate at that cadence: each start counts one update of nominal time
 * (1/rate, so the game's slow motion still slows particles); once that reaches 1/59.94 s the task
 * runs with the game's steps added up since its last full run, and otherwise it runs with a step of
 * 0, which redraws the particles where they are (with this frame's camera) without moving them.
 * Streaks are drawn as long as the step times the speed (the vertex builder's multiply at
 * 0x489349): that multiply reads a float in the patch data page instead, the last full run's step.
 * At 60 FPS every start is a full run. */
typedef void (ABI *ParticleStart)(void *task, float step);
static ParticleStart particle_start;
static float *particle_streak_step;
static void ABI particle_start_60(void *task, float step) {
    /* Each task's own cadence, by task address: the game makes and frees tasks as effects come
     * and go, so a task not in the table takes the least recently started entry. */
    static struct { void *task; uint64_t used; float nominal, pending; } tasks[64];
    static uint64_t starts;
    static atomic_flag lock=ATOMIC_FLAG_INIT;
    const float period=1.0f/59.94f, scale=atomic_load(&rate_scale);
    if (scale<=1.001f) {
        *particle_streak_step=step;
        particle_start(task,step);
        return;
    }
    while (atomic_flag_test_and_set_explicit(&lock,memory_order_acquire)) {}
    int t=0;
    for (int i=0;i<64;++i) {
        if (tasks[i].task==task) { t=i; break; }
        if (tasks[i].used<tasks[t].used) t=i;
    }
    if (tasks[t].task!=task) { tasks[t].task=task; tasks[t].nominal=tasks[t].pending=0.0f; }
    tasks[t].used=++starts;
    tasks[t].nominal+=period/scale;
    tasks[t].pending+=step;
    float run=0.0f;
    if (tasks[t].nominal+1e-6f>=period) {
        tasks[t].nominal-=period;
        if (tasks[t].nominal>period) tasks[t].nominal=0.0f; /* fell behind: don't run it twice */
        run=tasks[t].pending;
        tasks[t].pending=0.0f;
        *particle_streak_step=run;
    }
    atomic_flag_clear_explicit(&lock,memory_order_release);
    particle_start(task,run);
}
static void patch_particles(unsigned char *image, unsigned char *patch_data) {
    static const unsigned char mul[8]={0xc5,0xd2,0x59,0xb5,0x24,0xf8,0xff,0xff}; /* vmulss xmm6,xmm5,[rbp-0x7dc] */
    uint64_t *slot=(uint64_t *)(image+build->particle_start_slot);
    unsigned char *at=image+build->streak_mul;
    if (memcmp(at,mul,8) || *slot!=(uint64_t)(uintptr_t)(image+build->particle_start)) {
        printf("Particles: the particle task is not where eboot %s has it; shots look shorter above 60 FPS\n",build->version);
        return;
    }
    particle_streak_step=(float *)patch_data;
    *particle_streak_step=1.0f/59.94f;
    const int32_t rel=(int32_t)((intptr_t)patch_data-(intptr_t)(at+8));
    at[3]=0x35; /* vmulss xmm6,xmm5,[rip+rel] */
    memcpy(at+4,&rel,4);
    particle_start=(ParticleStart)(uintptr_t)*slot;
    *slot=(uint64_t)(uintptr_t)particle_start_60;
    puts("Particles: simulated at 59.94 Hz at any frame rate");
}

/* The renderer (Game_Renderer_Impl_ST vtable slot 2, 01.00 0x422d50) moves its scenes' animations
 * (the main menu's camera drift and background among them) once per drawn frame by a constant
 * 1/59.94 s (vmovss xmm0,[rip+...] at 0x422e1a and 0x422e80): at 240 FPS they ran 4x fast. Both
 * loads read a float in the patch data page instead, which apply_frame_rate keeps at 1/rate. */
static void patch_scene_step(unsigned char *image, unsigned char *patch_data) {
    int patched=0;
    scene_step=(float *)(patch_data+4);
    *scene_step=1.0f/59.94f;
    for (int i=0;i<2;++i) {
        unsigned char *at=image+build->scene_step[i];
        int32_t rel; memcpy(&rel,at+4,4);
        float value; memcpy(&value,at+8+rel,4);
        if (at[0]!=0xc5 || at[1]!=0xfa || at[2]!=0x10 || at[3]!=0x05 || value<0.0166f || value>0.0167f) continue;
        rel=(int32_t)((intptr_t)scene_step-(intptr_t)(at+8));
        memcpy(at+4,&rel,4);
        ++patched;
    }
    if (patched==2) puts("Scenes: menu animations follow the frame rate's step");
    else { scene_step=NULL; printf("Scenes: the renderer's frame step is not where eboot %s has it; menus animate faster above 60 FPS\n",build->version); }
}

/* Cheats (the overlay's Cheats page, L3+R3; stardust_cheats() bits, settings.ini cheats). Each one checked
 * against this build's bytes first, then applied from Player_Master_Impl's update (vtable slot 2,
 * 01.00 0x4d1fa0, the player's per-update step: f(this, float step)), on the game's own thread:
 * - invulnerable: the engine's own debug tunable sd3__game__god_mode (01.00 +0x9be37c), which
 *   the ship's hit handler (0x4d2f40) checks before it destroys the ship;
 * - infinite lives: the respawn (0x4d37c0) keeps the session's lives (Game_Session_Impl +0x84):
 *   `lea ecx,[rax-1]` becomes `lea ecx,[rax]` and its `jg` past the last-ship notice a `jmp`;
 * - infinite bombs: the smart bomb (0x4d5cb6) fires without `dec ecx` of the session's bombs
 *   (+0x98);
 * both keep at least one, so a ship or bomb is there when the cheat is turned on at none;
 * - max weapons: the three weapon levels (Player_Master_Impl +0x98, +0x9c, +0xa0) at their
 *   maximum_weaponN_power - 1, the top the game itself allows (0x4d2d37);
 * - boost: the session's boost cooldown (+0xb4, counted down by the session update 0x2c5972)
 *   zeroed, so the boost is ready again at once.
 * The session is the game's lookup (0x4b03e0) of Player_Master_Impl's +0xac; a session whose
 * +0x64 flag is set (lives_add, 0x2c4e06, leaves it alone) is left alone too. */
enum { CHEAT_INVULNERABLE=1, CHEAT_LIVES=2, CHEAT_BOMBS=4, CHEAT_WEAPONS=8, CHEAT_BOOST=16 };
typedef struct { uint32_t at; unsigned char off[3], on[3]; uint8_t size; } CodeCheat;
static CodeCheat code_cheats[3];
static int code_cheat_bits[3]={CHEAT_LIVES,CHEAT_LIVES,CHEAT_BOMBS}, code_cheats_ready, applied_cheats;
typedef void (ABI *MasterUpdate)(void *master, float step);
typedef unsigned char *(ABI *SessionById)(uint32_t id);
static MasterUpdate master_update;

/* Writes `n` bytes into the game's code, which is read and execute only. */
static void write_code(unsigned char *at, const unsigned char *bytes, size_t n) {
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(at,n,PAGE_EXECUTE_READWRITE,&old)) { printf("Cheats: code at %p not writable\n",(void *)at); return; }
    memcpy(at,bytes,n);
    VirtualProtect(at,n,old,&old);
    FlushInstructionCache(GetCurrentProcess(),at,n);
#else
    const uintptr_t page=(uintptr_t)sysconf(_SC_PAGESIZE), start=(uintptr_t)at&~(page-1);
    const size_t span=(uintptr_t)at+n-start;
    if (mprotect((void *)start,span,PROT_READ|PROT_WRITE|PROT_EXEC)) { printf("Cheats: code at %p not writable\n",(void *)at); return; }
    memcpy(at,bytes,n);
    mprotect((void *)start,span,PROT_READ|PROT_EXEC);
#endif
}

static void apply_code_cheats(int cheats) {
    for (int i=0;i<3;++i) {
        const int was=applied_cheats&code_cheat_bits[i], now=cheats&code_cheat_bits[i];
        if (!was==!now) continue;
        const CodeCheat *c=&code_cheats[i];
        write_code(game_image+c->at,now ? c->on : c->off,c->size);
    }
}

static void ABI master_update_cheats(void *master, float step) {
    const int cheats=stardust_cheats();
    if (cheats!=applied_cheats) {
        if (code_cheats_ready) apply_code_cheats(cheats);
        if ((cheats^applied_cheats)&CHEAT_INVULNERABLE) *(int32_t *)(game_image+build->god_mode)=(cheats&CHEAT_INVULNERABLE) ? 1 : 0;
        printf("Cheats:%s%s%s%s%s%s\n",cheats ? "" : " none",(cheats&CHEAT_INVULNERABLE) ? " invulnerable" : "",
               (cheats&CHEAT_LIVES) ? " lives" : "",(cheats&CHEAT_BOMBS) ? " bombs" : "",
               (cheats&CHEAT_WEAPONS) ? " weapons" : "",(cheats&CHEAT_BOOST) ? " boost" : "");
        applied_cheats=cheats;
    }
    if (cheats) {
        unsigned char *m=master;
        if (cheats&CHEAT_WEAPONS)
            for (int i=0;i<3;++i) {
                const int32_t top=*(int32_t *)(game_image+build->weapon_max[i])-1;
                if (top>0) *(int32_t *)(m+0x98+4*i)=top;
            }
        unsigned char *session=((SessionById)(uintptr_t)(game_image+build->session_by_id))(*(uint32_t *)(m+0xac));
        if (session && !session[0x64]) {
            if ((cheats&CHEAT_LIVES) && *(int32_t *)(session+0x84)<1) *(int32_t *)(session+0x84)=1;
            if ((cheats&CHEAT_BOMBS) && *(int32_t *)(session+0x98)<1) *(int32_t *)(session+0x98)=1;
            if ((cheats&CHEAT_BOOST) && *(float *)(session+0xb4)>0.0f) *(float *)(session+0xb4)=0.0f;
        }
    }
    master_update(master,step);
}

static void patch_cheats(unsigned char *image) {
    static const unsigned char lea[3]={0x8d,0x48,0xff}, jg=0x7f, dec[2]={0xff,0xc9};
    uint64_t *slot=(uint64_t *)(image+build->master_update_slot);
    if (*slot!=(uint64_t)(uintptr_t)(image+build->master_update)) {
        printf("Cheats: the player update is not where eboot %s has it; no cheats\n",build->version);
        return;
    }
    master_update=(MasterUpdate)(uintptr_t)*slot;
    *slot=(uint64_t)(uintptr_t)master_update_cheats;
    if (!memcmp(image+build->life_spend,lea,3) && image[build->life_last]==jg && !memcmp(image+build->bomb_spend,dec,2)) {
        code_cheats[0]=(CodeCheat){build->life_spend,{0x8d,0x48,0xff},{0x8d,0x48,0x00},3};
        code_cheats[1]=(CodeCheat){build->life_last,{0x7f},{0xeb},1};
        code_cheats[2]=(CodeCheat){build->bomb_spend,{0xff,0xc9},{0x66,0x90},2}; /* dec ecx / 2-byte nop */
        code_cheats_ready=1;
        puts("Cheats: ready (L3+R3)");
    } else {
        puts("Cheats: the respawn or smart bomb is not where this eboot has it; lives and bombs still top up at 1");
    }
}

void runtime_game_patch(unsigned char *image, unsigned char *patch_data) {
    if (!build) return;
    patch_cheats(image);
    patch_particles(image,patch_data);
    patch_scene_step(image,patch_data);
    patch_update_constants(image,patch_data);
    if (!strcmp(build->version,"01.00")) patch_page_fade(image,patch_data);
    patch_render_size(image);
    int patched=0;
    for (int i=0;i<2;++i) {
        unsigned char *at=image+build->hmd_sleep[i];
        int32_t rel; memcpy(&rel,at+1,4);
        if (at[0]!=0xe8 || build->hmd_sleep[i]+5+(int64_t)rel!=build->sleep_plt) {
            printf("Boot: +%#x holds %02x %02x %02x %02x %02x, not a call to +%#x\n",build->hmd_sleep[i],
                   at[0],at[1],at[2],at[3],at[4],build->sleep_plt);
            continue;
        }
        static const unsigned char nop5[5]={0x0f,0x1f,0x44,0x00,0x00};
        memcpy(at,nop5,5);
        ++patched;
    }
    printf("Boot: PS VR headset wait %s\n",patched==2 ? "removed" : "kept (not where this eboot has it)");
}

/* Other game-specific patches: none found in this eboot yet, so each hook leaves the game as it
 * is: the render device size (other aspect ratios show 16:9 with bars) and the camera jitter
 * (anti-aliasing stays on). */
int runtime_game_widen(unsigned char *image, unsigned char *patches, uint32_t width_4k) {
    (void)image; (void)patches; (void)width_4k;
    puts("Wide: not supported yet; other aspect ratios show the 16:9 picture with bars");
    return 0;
}
void runtime_game_antialiasing(unsigned char *image, unsigned char *patches, unsigned char *patch_data) {
    (void)image; (void)patches; (void)patch_data;
}
