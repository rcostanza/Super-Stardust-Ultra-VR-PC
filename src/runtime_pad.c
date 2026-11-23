/* libScePad on SDL3 gamepads, with the keyboard as a second source. SDL events
 * are pumped by the window thread (gpu/shim/window.cpp); here state is only sampled, on the pad's
 * own host thread (host_thread).
 *
 * Two players for local co-op: the first gamepad found (and the keyboard) is user 1's pad, the
 * second user 2's, and each keeps its slot until it disconnects. The second pad's arrival logs in
 * user 2 (runtime_pad_users, runtime_services.c), and the game opens a pad for that user as it
 * would for a second controller signing in on the console.
 *
 * Gamepads map by position: the bottom face button is Cross, right Circle, left Square, top
 * Triangle (an Xbox pad's A B X Y). View/Back clicks the touchpad; a DualSense's own touchpad works.
 *
 * The game's pad speaker sound plays on a USB DualShock 4's or DualSense's speaker
 * (runtime_audio.c); a DualSense is told here to send its USB audio there (speaker_path).
 *
 * Keyboard layout, by key position:
 *   WASD left stick, IJKL right stick, arrow keys d-pad (menus),
 *   Space/Enter Cross, Escape/Backspace/; Circle, F Square, H Triangle,
 *   Q L1, O R1, E L2, U R2 (Page Up/Page Down are L1/R1 too), X L3, M R3,
 *   Tab/P Options, T touchpad click. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/stardust_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PLAYERS 2 /* user n's pad is handle n */

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a touchpad click from a button or key without a
     * touch surface (play_click turns it into a touch and a click), and the keyboard's sticks. */
    CLICK=0x80000000,
    LSTICK_UP=0x00010000, LSTICK_DOWN=0x00020000, LSTICK_LEFT=0x00040000, LSTICK_RIGHT=0x00080000,
    RSTICK_UP=0x00200000, RSTICK_DOWN=0x00400000, RSTICK_LEFT=0x00800000, RSTICK_RIGHT=0x01000000,
    STICK_DIRECTIONS=LSTICK_UP|LSTICK_DOWN|LSTICK_LEFT|LSTICK_RIGHT|RSTICK_UP|RSTICK_DOWN|RSTICK_LEFT|RSTICK_RIGHT,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened[PLAYERS], sdl_ready;
static size_t reads, vibrations;
/* The host side runs on its own thread (host_thread): opening, closing and even reading an SDL
 * gamepad can block for hundreds of ms while a Bluetooth pad connects or drops (SDL holds its
 * joystick lock meanwhile), and on the game's thread that froze the frame. The thread samples every
 * millisecond and a read copies the latest sample, so the game's threads make no gamepad calls. */
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint8_t large, small; uint32_t serial; } Rumble;
static PadData host_sample[PLAYERS];  /* under host_lock */
static char gamepad_name[PLAYERS][64]={"none","none"}; /* under host_lock */
static Rumble rumble[PLAYERS];        /* under host_lock */
static _Atomic int captured_since_read[PLAYERS]; /* the host thread saw a dialog open since that pad's last read */
static int reconnect_wanted;          /* atomic: the pad file's reconnect token */
static _Atomic int second_user;       /* a second gamepad has connected: user 2 is logged in */
static _Atomic int speaker_wanted;    /* runtime_pad_speaker_route */
static _Atomic int speaker_restored;  /* the host thread has put every DualSense back (exit report) */

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
/* Pad calls run on the game's own threads, so a slow one is a late frame: calls over 2 ms are
 * counted per kind (read, vibration, info, and the host thread's sample) and the first 20 printed.
 * One thread notes each kind; the shared count and start time are atomic. */
enum { SLOW_READ, SLOW_VIBRATION, SLOW_INFO, SLOW_HOST, SLOW_KINDS };
static size_t slow_calls[SLOW_KINDS], slow_shown;
static uint64_t slow_max_us[SLOW_KINDS], slow_epoch;
static void note_call(int kind, uint64_t start) {
    static const char *const names[SLOW_KINDS]={"read","vibration","info","host sample"};
    const uint64_t us=now_us()-start;
    uint64_t unset=0;
    __atomic_compare_exchange_n(&slow_epoch,&unset,start,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
    if (us<2000) return;
    ++slow_calls[kind];
    if (us>slow_max_us[kind]) slow_max_us[kind]=us;
    if (__atomic_fetch_add(&slow_shown,1,__ATOMIC_RELAXED)<20)
        printf("Runtime: slow pad %s %.1f ms at %.3f s\n",names[kind],us/1e3,(start-__atomic_load_n(&slow_epoch,__ATOMIC_RELAXED))/1e6);
}
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, the T key, the pad file) is played like a finger in
 * the middle of the touchpad: the touch is reported CLICK_LEAD_US before the button, the button
 * stays at least CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release, as a real
 * finger would. */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
typedef struct { bool active; uint64_t pressed, released; } Click;
static Click clicks[PLAYERS]; /* under lock */
static void play_click(PadData *d, Click *c) {
    const uint64_t now=d->timestamp;
    bool source=(d->buttons & CLICK)!=0;
    d->buttons&=~CLICK;
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        source=true;
    }
    if (source && (!c->active || c->released)) { c->active=true; c->pressed=now; c->released=0; }
    else if (!source && c->active && !c->released) c->released=now;
    if (!c->active) return;
    const uint64_t down=c->pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!c->released) up=UINT64_MAX;
    else if (c->released>up) up=c->released;
    if (now>=up && now-up>=CLICK_TAIL_US) { c->active=false; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=960,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* Host thread: the players' gamepads (g[0] player 1, g[1] player 2), closing one when it is gone
 * (or every one when the pad file asks for a reconnect), and every 100 ms while a slot is empty,
 * giving the first empty slot each gamepad not open yet. A pad keeps its slot while connected, so
 * player 1's dropping leaves player 2 as player 2. */
static void host_gamepads(SDL_Gamepad **g, uint64_t now, uint64_t *next_scan, uint8_t *connected_count) {
    const int reconnect=__atomic_exchange_n(&reconnect_wanted,0,__ATOMIC_ACQ_REL);
    bool empty=false;
    for (int p=0;p<PLAYERS;++p) {
        if (g[p] && (reconnect || !SDL_GamepadConnected(g[p]))) {
            SDL_CloseGamepad(g[p]); g[p]=NULL;
            pthread_mutex_lock(&host_lock); strcpy(gamepad_name[p],"none"); pthread_mutex_unlock(&host_lock);
            printf("Runtime: player %d gamepad closed\n",p+1);
        }
        empty|=!g[p];
    }
    if (!empty || now<*next_scan) return;
    *next_scan=now+100000;
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int i=0;ids && i<count;++i) {
        int slot=-1;
        for (int p=PLAYERS-1;p>=0;--p) {
            if (g[p] && SDL_GetGamepadID(g[p])==ids[i]) { slot=-1; break; }
            if (!g[p]) slot=p;
        }
        if (slot<0 || !(g[slot]=SDL_OpenGamepad(ids[i]))) continue;
        ++connected_count[slot];
        SDL_SetGamepadPlayerIndex(g[slot],slot); /* the pad's own player light, where it has one */
        const char *name=SDL_GetGamepadName(g[slot]);
        pthread_mutex_lock(&host_lock);
        snprintf(gamepad_name[slot],sizeof(gamepad_name[slot]),"%s",name ? name : "gamepad");
        pthread_mutex_unlock(&host_lock);
        printf("Runtime: player %d gamepad connected: %s\n",slot+1,name ? name : "gamepad");
        if (slot==1 && !atomic_exchange(&second_user,1)) puts("Runtime: second gamepad; user 2 logs in");
    }
    SDL_free(ids);
}
int runtime_pad_users(void) { return atomic_load(&second_user) ? 2 : 1; }
void runtime_pad_speaker_route(int on) { atomic_store(&speaker_wanted,on ? 1 : 0); }

/* A wired DualSense plays the first two channels of its USB audio on its headphone jack. Its
 * output report (SDL's DS5EffectsState_t, sent as it is by SDL_SendGamepadEffect) switches that:
 * byte 0 bit 0x80 allows the audio control byte 7, whose bits 4-5 are the output path (0 headphones
 * left/right, 3 the right channel on the speaker); bit 0x20 allows the speaker volume, byte 5
 * (the PS5 uses 0x3d-0x64). Byte 38 bit 0x04 keeps SDL's improved rumble emulation on.
 * Over Bluetooth there is no audio device, so only wired pads are switched. */
static bool speaker_path(SDL_Gamepad *g, int speaker) {
    uint8_t effect[47]={0};
    effect[0]=speaker ? 0xa0 : 0x80;
    effect[5]=0x64;
    effect[7]=speaker ? 0x30 : 0x00;
    effect[38]=0x04;
    return SDL_SendGamepadEffect(g,effect,sizeof(effect));
}
/* Host thread: each wired DualSense's output path follows speaker_wanted; a pad that (re)connects
 * starts on its headphones, as a DualSense does when it powers up. `sent` and `seen` per slot. */
static void host_speakers(SDL_Gamepad *const *g, int *sent, SDL_Gamepad **seen) {
    const int want=atomic_load(&speaker_wanted);
    for (int p=0;p<PLAYERS;++p) {
        if (g[p]!=seen[p]) { seen[p]=g[p]; sent[p]=0; }
        if (!g[p] || sent[p]==want || SDL_GetGamepadType(g[p])!=SDL_GAMEPAD_TYPE_PS5
            || SDL_GetGamepadConnectionState(g[p])!=SDL_JOYSTICK_CONNECTION_WIRED) continue;
        const bool ok=speaker_path(g[p],want);
        printf(ok ? "Runtime: player %d DualSense sound to its %s\n" : "Runtime: player %d DualSense sound to its %s failed: %s\n",
               p+1,want ? "speaker" : "headphone jack",SDL_GetError());
        sent[p]=want; /* tried once, not every millisecond */
    }
    if (!want) atomic_store(&speaker_restored,1);
}
/* PS4 button names for scripted input (STARDUST_PAD_FILE). */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    {"lstick_up",LSTICK_UP}, {"lstick_down",LSTICK_DOWN}, {"lstick_left",LSTICK_LEFT}, {"lstick_right",LSTICK_RIGHT},
    {"rstick_up",RSTICK_UP}, {"rstick_down",RSTICK_DOWN}, {"rstick_left",RSTICK_LEFT}, {"rstick_right",RSTICK_RIGHT},
};
/* Button sources: a gamepad button or key and the PS4 button it presses. Gamepads map by
 * position; the keyboard layout is the one in the comment at the top. */
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[32]; size_t count; } ButtonMap;
static const ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static const ButtonMap key_map={{
    {SDL_SCANCODE_W,LSTICK_UP}, {SDL_SCANCODE_S,LSTICK_DOWN}, {SDL_SCANCODE_A,LSTICK_LEFT}, {SDL_SCANCODE_D,LSTICK_RIGHT},
    {SDL_SCANCODE_I,RSTICK_UP}, {SDL_SCANCODE_K,RSTICK_DOWN}, {SDL_SCANCODE_J,RSTICK_LEFT}, {SDL_SCANCODE_L,RSTICK_RIGHT},
    {SDL_SCANCODE_UP,BTN_UP}, {SDL_SCANCODE_DOWN,BTN_DOWN}, {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_RETURN,BTN_CROSS}, {SDL_SCANCODE_KP_ENTER,BTN_CROSS},
    {SDL_SCANCODE_ESCAPE,BTN_CIRCLE}, {SDL_SCANCODE_BACKSPACE,BTN_CIRCLE}, {SDL_SCANCODE_SEMICOLON,BTN_CIRCLE},
    {SDL_SCANCODE_F,BTN_SQUARE}, {SDL_SCANCODE_H,BTN_TRIANGLE},
    {SDL_SCANCODE_Q,BTN_L1}, {SDL_SCANCODE_PAGEUP,BTN_L1}, {SDL_SCANCODE_O,BTN_R1}, {SDL_SCANCODE_PAGEDOWN,BTN_R1},
    {SDL_SCANCODE_E,BTN_L2}, {SDL_SCANCODE_U,BTN_R2}, {SDL_SCANCODE_X,BTN_L3}, {SDL_SCANCODE_M,BTN_R3},
    {SDL_SCANCODE_TAB,BTN_OPTIONS}, {SDL_SCANCODE_P,BTN_OPTIONS}, {SDL_SCANCODE_T,CLICK},
},31};

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}

/* keys: SDL keyboard state. Returns the mapped sources held (0: the keyboard is idle). */
static uint32_t sample_keyboard(PadData *d, const bool *keys) {
    uint32_t pressed=0;
    for (size_t i=0;i<key_map.count;++i) {
        const int source=key_map.entries[i].source;
        if (keys[source]) pressed|=key_map.entries[i].ps;
    }
    d->buttons|=pressed & ~STICK_DIRECTIONS;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,pressed & LSTICK_LEFT,pressed & LSTICK_RIGHT);
    key_axis(&d->left_y,pressed & LSTICK_UP,pressed & LSTICK_DOWN);
    key_axis(&d->right_x,pressed & RSTICK_LEFT,pressed & RSTICK_RIGHT);
    key_axis(&d->right_y,pressed & RSTICK_UP,pressed & RSTICK_DOWN);
    return pressed;
}

/* Host thread: player's gamepad, and for player 1 the keyboard, as one PadData (no timestamp).
 * Player 1 always reads connected (the keyboard is there); player 2 only while its pad is. */
static void host_read(PadData *d, int player, SDL_Gamepad *g, uint8_t connected_count) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=!player || g; d->connected_count=connected_count ? connected_count : 1;
    if (stardust_gpu_overlay_captures_input()) { /* a dialog is open: neutral input */
        atomic_store(&captured_since_read[player],1);
        return;
    }
    const bool *k=!player && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (k) sample_keyboard(d,k);
}

/* Host thread: Select (Back/View/Share) or Start (Options/Menu) held alone for MENU_HOLD_US opens
 * the Display Settings panel; Select+Start opens the exit prompt, as F4. What each presses (the
 * touchpad click, Options) is held back while it is down and played as a MENU_TAP_US tap when it
 * is let go sooner. It is dropped once the hold opened the panel or prompt, or when the press began
 * while an overlay had the input or within MENU_GRACE_US of it (the press that closed it). Each
 * player's pad has its own holds. */
enum { MENU_HOLD_US=1000000, MENU_TAP_US=60000, MENU_GRACE_US=100000 };
typedef struct { SDL_GamepadButton button; const char *name; uint64_t since, tap_until; bool opened; } MenuHold;
typedef struct { MenuHold holds[2]; uint64_t captured_at; bool both_was; } MenuHolds;
#define MENU_HOLDS {{{SDL_GAMEPAD_BUTTON_BACK,"Select",0,0,false},{SDL_GAMEPAD_BUTTON_START,"Start",0,0,false}},0,false}
static void menu_hold(PadData *d, SDL_Gamepad *g, uint64_t now, MenuHolds *state) {
    MenuHold *holds=state->holds;
    uint64_t *captured_at=&state->captured_at;
    bool *both_was=&state->both_was;
    const bool captured=stardust_gpu_overlay_captures_input();
    if (captured) *captured_at=now;
    bool down[2];
    uint32_t bits[2]={0,0};
    for (int i=0;i<2;++i) {
        MenuHold *h=&holds[i];
        for (size_t j=0;j<pad_map.count;++j)
            if (pad_map.entries[j].source==(int)h->button) bits[i]|=pad_map.entries[j].ps;
        down[i]=g && SDL_GetGamepadButton(g,h->button);
        if (down[i]) {
            if (!h->since) { h->since=now; h->opened=*captured_at && now-*captured_at<MENU_GRACE_US; }
            if (captured) h->opened=true;
            d->buttons&=~bits[i];
        } else {
            if (h->since && !h->opened) h->tap_until=now+MENU_TAP_US;
            h->since=0;
            if (now<h->tap_until) d->buttons|=bits[i];
        }
    }
    const bool both=down[0] && down[1];
    if (both && !*both_was && !holds[0].opened && !holds[1].opened) {
        holds[0].opened=holds[1].opened=true;
        stardust_exit_prompt_open();
        puts("Runtime: Select+Start; asking whether to close the game");
    }
    *both_was=both;
    for (int i=0;i<2;++i) {
        MenuHold *h=&holds[i];
        if (!down[i] || down[1-i] || h->opened || now-h->since<MENU_HOLD_US) continue;
        h->opened=true;
        if (!stardust_display_settings_is_open()) {
            stardust_display_settings_open();
            printf("Runtime: %s held; Display Settings opened\n",h->name);
        }
    }
}

/* Host thread: L3+R3 (both sticks clicked, on either player's pad; on the keyboard X+M, their keys)
 * opens the Cheats page, or closes it, once per press of the pair; another page with the input
 * keeps it shut. */
static bool sticks_clicked(SDL_Gamepad *g) {
    return g && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK) && SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_STICK);
}
static void cheats_combo(SDL_Gamepad *const *g) {
    static bool both_was;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    bool both=k && k[SDL_SCANCODE_X] && k[SDL_SCANCODE_M];
    for (int p=0;p<PLAYERS;++p) both|=sticks_clicked(g[p]);
    if (both && !both_was) {
        stardust_cheats_toggle();
        puts(stardust_cheats_is_open() ? "Runtime: L3+R3; Cheats opened" : "Runtime: L3+R3; Cheats closed");
    }
    both_was=both;
}

/* Host thread: samples every millisecond (SDL_DelayNS waits on a high resolution timer) and
 * passes each pad's vibration on: when it changes, and every 500 ms while the motors run, as each
 * rumble is sent for 1000 ms. */
static void *host_thread(void *arg) {
    (void)arg;
    SDL_Gamepad *g[PLAYERS]={0};
    uint64_t next_scan=0, rumble_sent[PLAYERS]={0};
    uint32_t rumble_serial[PLAYERS]={0};
    uint8_t connected_count[PLAYERS]={0};
    MenuHolds holds[PLAYERS]={MENU_HOLDS,MENU_HOLDS};
    int speaker_sent[PLAYERS]={0}; SDL_Gamepad *speaker_seen[PLAYERS]={0};
    for (;;) {
        const uint64_t start=now_us();
        if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        if (sdl_ready>0) host_gamepads(g,start,&next_scan,connected_count);
        if (sdl_ready>0) host_speakers(g,speaker_sent,speaker_seen);
        PadData d[PLAYERS];
        for (int p=0;p<PLAYERS;++p) {
            host_read(&d[p],p,g[p],connected_count[p]);
            menu_hold(&d[p],g[p],start,&holds[p]);
        }
        cheats_combo(g);
        Rumble want[PLAYERS];
        pthread_mutex_lock(&host_lock);
        memcpy(host_sample,d,sizeof(d));
        memcpy(want,rumble,sizeof(want));
        pthread_mutex_unlock(&host_lock);
        for (int p=0;p<PLAYERS;++p) {
            const Rumble *r=&want[p];
            if (g[p] && (r->serial!=rumble_serial[p] || ((r->large || r->small) && start-rumble_sent[p]>=500000))) {
                SDL_RumbleGamepad(g[p],(uint16_t)(r->large*257u),(uint16_t)(r->small*257u),1000);
                rumble_serial[p]=r->serial; rumble_sent[p]=start;
            }
        }
        note_call(SLOW_HOST,start);
        SDL_DelayNS(1000000);
    }
    return NULL;
}

/* STARDUST_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad up down left right) are held while listed; lx= ly= rx= ry= (0..255) override
 * the sticks; reconnect closes the gamepad once per edit and the host thread opens
 * it again, as after a real reconnect; settings (or settings=<row>) opens the Display Settings
 * panel once per edit, as F10 does, with that row focused; cheats opens the Cheats page as L3+R3
 * does, cheats=<bits> ticks those cheats (src/runtime_game.c CHEAT_*) without it. An empty file
 * releases everything. */
static struct { uint32_t buttons; int stick[4]; } injected={0,{-1,-1,-1,-1}};
static int replay_armed;      /* 1 while a STARDUST_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("STARDUST_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    int reconnect=0;
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* STARDUST_PAD_REPLAY */
        if (!strcmp(token,"reconnect")) reconnect=1;
        if (!strncmp(token,"settings",8) && (!token[8] || token[8]=='=')) stardust_gpu_open_display_settings(token[8] ? atoi(token+9) : 0);
        if (!strcmp(token,"cheats")) stardust_cheats_open();
        if (!strncmp(token,"cheats=",7)) stardust_gpu_set_cheats(atoi(token+7));
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    if (reconnect) __atomic_store_n(&reconnect_wanted,1,__ATOMIC_RELEASE); /* host_gamepad closes it */
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* STARDUST_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; STARDUST_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in STARDUST_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("STARDUST_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("STARDUST_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}

/* settings.ini skip_intro (runtime_game_in_intro, runtime_game.c): D3T's logo movie ends after its
 * first frame, as if it were over (AvPlayer reports it, `playing` 1, and 0 when it closes; the
 * return value ends it), and while the start-up screens show Cross is tapped for 100 ms every
 * 400 ms, as the player would on the software usage terms, which wait for it. The taps end when
 * the menu music opens, on the title screen, which one more press passes. */
static _Atomic int logo_seen;
int runtime_pad_logo_movie(int playing) {
    if (!playing || !runtime_game_in_intro() || atomic_exchange(&logo_seen,1)) return 0;
    puts("Intro: D3T logo movie; ended after its first frame (skip_intro=1)");
    return 1;
}
static void intro_press(PadData *d) {
    static uint64_t since, title_since;
    static int title_done;
    if (title_done || !stardust_skip_intro()) return;
    const uint64_t now=now_us();
    if (runtime_game_in_intro()) {
        if (!since) since=now;
        if ((now-since)%400000<100000) d->buttons|=BTN_CROSS;
        return;
    }
    /* The title screen ("Press X button") takes Cross once it has faded in: one press 1.5 s
     * after the menu music opens, so the start lands in the main menu. */
    if (!title_since) title_since=now;
    if (now-title_since<1500000) return;
    if (now-title_since<1600000) { d->buttons|=BTN_CROSS; return; }
    title_done=1;
    puts("Intro: Cross pressed on the title screen (skip_intro=1)");
}

/* After a dialog closes, buttons still held (the Cross that accepted a name) stay hidden until
 * released: the game would take them as a new press. Scripted input (pad file, recording, replay)
 * and the intro's presses are player 1's. */
static int hold_after_capture[PLAYERS];
static void sample(PadData *d, int player) {
    pthread_mutex_lock(&host_lock);
    *d=host_sample[player];
    pthread_mutex_unlock(&host_lock);
    if (!d->connected_count) { /* before the host thread's first sample */
        d->left_x=d->left_y=d->right_x=d->right_y=128;
        d->orientation[3]=1.0f;
        d->connected=!player; d->connected_count=1;
    }
    d->timestamp=now_us();
    /* Display Settings freezes the game, which reads no pad meanwhile: a dialog seen by the host
     * thread since the last read counts as one open now. */
    Click *c=&clicks[player];
    if (atomic_exchange(&captured_since_read[player],0)) { hold_after_capture[player]=1; c->active=false; }
    if (stardust_gpu_overlay_captures_input()) { hold_after_capture[player]=1; c->active=false; return; }
    if (!player) {
        record_sample(d); /* the click sources as they are: a replay plays them again */
        read_inject();
        replay_sample(d);
        d->buttons|=injected.buttons;
        if (injected.buttons & BTN_L2) d->l2=255;
        if (injected.buttons & BTN_R2) d->r2=255;
        uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
        for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    }
    play_click(d,c);
    if (!player) intro_press(d);
    if (hold_after_capture[player]) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture[player]=0;
    }
}

static void start_host_thread(void) {
    pthread_t thread;
    if (pthread_create(&thread,NULL,host_thread,NULL)==0) pthread_detach(thread);
    else puts("Runtime: pad host thread failed to start; the pad reads neutral");
}
/* The player a handle names (handle n is user n's pad), or -1 when it is not open. */
static int handle_player(int32_t handle) {
    return handle>=1 && handle<=PLAYERS && __atomic_load_n(&opened[handle-1],__ATOMIC_ACQUIRE) ? handle-1 : -1;
}
static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
/* scePadOpen(user, type, index, param): one standard (0) or special (2) port per logged-in user;
 * user 2 exists once a second gamepad has connected (runtime_pad_users). */
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<1 || user>runtime_pad_users()) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened[user-1];
    __atomic_store_n(&opened[user-1],1,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    static pthread_once_t host_started=PTHREAD_ONCE_INIT;
    pthread_once(&host_started,start_host_thread);
    printf(user==1 ? "Runtime: pad opened for user 1 (SDL gamepad or keyboard)\n"
                   : "Runtime: pad opened for user %d (second SDL gamepad)\n",user);
    return user;
}
static ABI int32_t pad_close(int32_t handle) {
    const int player=handle_player(handle);
    if (player<0) return ERR_INVALID_HANDLE;
    __atomic_store_n(&opened[player],0,__ATOMIC_RELEASE); return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    const int player=handle_player(handle);
    if (player<0) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    const uint64_t start=now_us();
    pthread_mutex_lock(&lock);
    sample(data,player); ++reads;
    note_call(SLOW_READ,start);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    const int player=handle_player(handle);
    if (player<0) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->device_class=0;
    const uint64_t start=now_us();
    pthread_mutex_lock(&host_lock);
    const PadData *s=&host_sample[player];
    info->connected=s->connected_count ? s->connected : !player;
    info->connected_count=s->connected_count ? s->connected_count : 1;
    pthread_mutex_unlock(&host_lock);
    note_call(SLOW_INFO,start);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    const int player=handle_player(handle);
    if (player<0) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    const uint64_t start=now_us();
    pthread_mutex_lock(&host_lock); /* host_thread sends it */
    Rumble *r=&rumble[player];
    if (r->large!=param[0] || r->small!=param[1]) { r->large=param[0]; r->small=param[1]; ++r->serial; }
    pthread_mutex_unlock(&host_lock);
    note_call(SLOW_VIBRATION,start);
    __atomic_add_fetch(&vibrations,1,__ATOMIC_RELAXED);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle_player(handle)>=0 ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle_player(handle)<0) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) {
    /* A DualSense keeps its output path until it powers off: its headphone jack is given back
       before the game ends (up to 200 ms for the host thread). */
    if (atomic_exchange(&speaker_wanted,0)) {
        atomic_store(&speaker_restored,0);
        for (int i=0;i<200 && !atomic_load(&speaker_restored);++i) SDL_DelayNS(1000000);
    }
    char name[PLAYERS][sizeof(gamepad_name[0])];
    pthread_mutex_lock(&host_lock); memcpy(name,gamepad_name,sizeof(name)); pthread_mutex_unlock(&host_lock);
    printf("Runtime: pad reads=%zu, vibrations=%zu, gamepads=%s / %s; over 2 ms: reads %zu (max %.1f ms), "
           "vibrations %zu (max %.1f ms), info %zu (max %.1f ms), host samples %zu (max %.1f ms)\n",reads,vibrations,name[0],name[1],
           slow_calls[SLOW_READ],slow_max_us[SLOW_READ]/1e3,slow_calls[SLOW_VIBRATION],slow_max_us[SLOW_VIBRATION]/1e3,
           slow_calls[SLOW_INFO],slow_max_us[SLOW_INFO]/1e3,slow_calls[SLOW_HOST],slow_max_us[SLOW_HOST]/1e3);
}

/* stardust --gamepad: runs before any game state exists, so it uses its own SDL gamepad and leaves
 * the lock and statics above alone. */
static const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return "unknown";
    }
}
int runtime_pad_describe(void) {
    SDL_Gamepad *g=NULL;
    const bool ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    /* No event loop yet, so pads show up only after a pump: poll for about 300 ms. */
    for (int waited=0; ready && !g && waited<30; ++waited) {
        SDL_PumpEvents();
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) g=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!g) SDL_Delay(10);
    }
    if (!g) puts("Gamepad: none");
    else {
        const char *type=SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const char *name=SDL_GetGamepadName(g);
        printf("Gamepad: type=%s south=%s east=%s west=%s north=%s name=%s\n",
               type ? type : "unknown",
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_EAST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_WEST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_NORTH)),
               name ? name : "unknown");
        SDL_CloseGamepad(g);
    }
    if (ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return g!=NULL;
}
