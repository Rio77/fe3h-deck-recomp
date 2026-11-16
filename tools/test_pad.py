from pathlib import Path
import subprocess
s=Path('runtime/ui/menu.mm').read_text();s=s.split('case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {',1)[1].split('        if (!capturing()) return false;',1)[0]
prefix=r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#define LOG(...) fprintf(stderr,__VA_ARGS__)
enum {SDL_EVENT_GAMEPAD_BUTTON_DOWN=1,SDL_EVENT_GAMEPAD_BUTTON_UP,SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_GUIDE,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
using SDL_GamepadButton=int;struct SDL_Gamepad{} pad;
struct Event{int type;struct{int button,which;}gbutton;};
bool L,R,back_down,start_down,typing;std::atomic<bool>g_open;
struct{bool pad_menu_combo=true;}g_settings;
SDL_Gamepad*SDL_GetGamepadFromID(int){return &pad;}
bool SDL_GetGamepadButton(SDL_Gamepad*,int b){return b==SDL_GAMEPAD_BUTTON_LEFT_SHOULDER?L:R;}
const char*SDL_GetGamepadStringForButton(int){return "test";}
void set_open(bool v){g_open=v;}
bool handle(const Event*e){
'''
suffix=r'''
return false;}
void reset(){L=R=back_down=start_down=typing=false;g_open=false;g_settings.pad_menu_combo=true;}
bool press(int button){Event e={SDL_EVENT_GAMEPAD_BUTTON_DOWN,{button,0}};return handle(&e);}
int main(){
 reset();assert(!press(SDL_GAMEPAD_BUTTON_BACK));assert(press(SDL_GAMEPAD_BUTTON_START));assert(g_open);
 for(int r=0;r<2;r++){reset();L=!r;R=r;assert(!press(SDL_GAMEPAD_BUTTON_BACK));assert(!press(SDL_GAMEPAD_BUTTON_START));assert(!press(SDL_GAMEPAD_BUTTON_GUIDE));assert(!g_open);}
 reset();g_settings.pad_menu_combo=false;assert(!press(SDL_GAMEPAD_BUTTON_BACK));assert(!press(SDL_GAMEPAD_BUTTON_START));assert(!press(SDL_GAMEPAD_BUTTON_GUIDE));assert(!g_open);
 reset();g_settings.pad_menu_combo=false;assert(press(SDL_GAMEPAD_BUTTON_GUIDE));assert(g_open);
 reset();typing=true;press(SDL_GAMEPAD_BUTTON_BACK);assert(!press(SDL_GAMEPAD_BUTTON_START));assert(!g_open);
 reset();Event up={SDL_EVENT_GAMEPAD_BUTTON_UP,{SDL_GAMEPAD_BUTTON_GUIDE,0}};assert(!handle(&up));assert(!g_open);
 puts("PASS: actual menu event branch: normal chord, left/right shoulder reset pass-through, disabled chord, synthetic Guide, standalone Guide, typing, button release");}
'''
p=Path('/tmp/test_pad_opt4.cpp');p.write_text(prefix+s+suffix)
subprocess.run(['clang++','-std=c++17',str(p),'-o','/tmp/test_pad_opt4'],check=True)
subprocess.run(['/tmp/test_pad_opt4'],check=True)
