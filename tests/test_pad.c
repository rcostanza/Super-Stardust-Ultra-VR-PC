/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pad mapping: the keyboard layout, the gamepad's Back button and the second player's pad. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_pad.c"

int stardust_gpu_overlay_captures_input(void) { return 0; }
int stardust_skip_intro(void) { return 0; }
int runtime_game_in_intro(void) { return 0; }
int stardust_display_settings_is_open(void) { return 0; }
void stardust_display_settings_open(void) {}
void stardust_exit_prompt_open(void) {}
void stardust_cheats_open(void) {}
void stardust_cheats_toggle(void) {}
int stardust_cheats_is_open(void) { return 0; }
void stardust_gpu_set_cheats(int bits) { (void)bits; }
void stardust_gpu_open_display_settings(int row) { (void)row; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static bool keys[SDL_SCANCODE_COUNT];

/* The keyboard's contribution to a neutral pad with the keys in `held` down. */
static PadData press(const SDL_Scancode *held, size_t count) {
    memset(keys,0,sizeof(keys));
    for (size_t i=0;i<count;++i) keys[held[i]]=true;
    PadData d;
    memset(&d,0,sizeof(d));
    d.left_x=d.left_y=d.right_x=d.right_y=128;
    sample_keyboard(&d,keys);
    return d;
}
static PadData press1(SDL_Scancode key) { return press(&key,1); }

static size_t sources_of(const ButtonMap *map, uint32_t ps) {
    size_t n=0;
    for (size_t i=0;i<map->count;++i) n+=map->entries[i].ps==ps;
    return n;
}

static void test_default_keyboard(void) {
    assert(press1(SDL_SCANCODE_SPACE).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_RETURN).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_ESCAPE).buttons==BTN_CIRCLE);
    assert(press1(SDL_SCANCODE_F).buttons==BTN_SQUARE);
    assert(press1(SDL_SCANCODE_H).buttons==BTN_TRIANGLE);
    assert(press1(SDL_SCANCODE_Q).buttons==BTN_L1 && press1(SDL_SCANCODE_O).buttons==BTN_R1);
    assert(press1(SDL_SCANCODE_UP).buttons==BTN_UP && press1(SDL_SCANCODE_RIGHT).buttons==BTN_RIGHT);
    assert(press1(SDL_SCANCODE_TAB).buttons==BTN_OPTIONS);
    assert(press1(SDL_SCANCODE_T).buttons==CLICK); /* play_click turns it into a touch */
    PadData d=press1(SDL_SCANCODE_E);
    assert(d.buttons==BTN_L2 && d.l2==255);
    /* Sticks: no button bits, full tilt, both sticks at once. */
    const SDL_Scancode slide[]={SDL_SCANCODE_W,SDL_SCANCODE_D,SDL_SCANCODE_J};
    d=press(slide,3);
    assert(d.buttons==0);
    assert(d.left_y==0 && d.left_x==255 && d.right_x==0 && d.right_y==128);
    /* Two buttons at once, one from each hand. */
    const SDL_Scancode taps[]={SDL_SCANCODE_F,SDL_SCANCODE_H};
    assert(press(taps,2).buttons==(BTN_SQUARE|BTN_TRIANGLE));
    assert(press(NULL,0).buttons==0);
}

static void test_pad_buttons(void) {
    assert(sources_of(&pad_map,CLICK)==1); /* Back/View clicks the touchpad */
    assert(sources_of(&pad_map,BTN_CROSS)==1 && sources_of(&key_map,BTN_CROSS)==3);
}

/* Player 2: no keyboard, disconnected until its own gamepad is there, its own handle. */
static void test_second_player(void) {
    PadData d;
    host_read(&d,0,NULL,0);
    assert(d.connected==1 && d.connected_count==1 && d.left_x==128);
    host_read(&d,1,NULL,0);
    assert(d.connected==0 && d.buttons==0 && d.left_x==128 && d.right_y==128);
    sample(&d,1); /* before any host sample: neutral and disconnected */
    assert(d.connected==0 && d.left_x==128 && d.orientation[3]==1.0f);
    sample(&d,0);
    assert(d.connected==1);
    assert(runtime_pad_users()==1);
    atomic_store(&second_user,1);
    assert(runtime_pad_users()==2);
    initialized=1;
    assert(handle_player(1)<0 && handle_player(2)<0);
    opened[0]=opened[1]=1;
    assert(handle_player(1)==0 && handle_player(2)==1 && handle_player(3)<0 && handle_player(0)<0);
    const uint8_t motors[2]={200,50};
    assert(pad_vibration(2,motors)==0);
    assert(rumble[1].large==200 && rumble[1].small==50 && rumble[0].large==0);
    assert(pad_close(2)==0 && handle_player(2)<0 && pad_read_state(2,&d)==ERR_INVALID_HANDLE);
    opened[0]=0; atomic_store(&second_user,0); initialized=0;
}

int main(void) {
    test_default_keyboard();
    test_pad_buttons();
    test_second_player();
    puts("pad tests passed");
    return 0;
}
