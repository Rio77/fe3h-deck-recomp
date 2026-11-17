#include <atomic>
#include <cassert>
#include <thread>
#include "ui/text_clipboard.h"
int main() {
    assert(SDL_Init(SDL_INIT_VIDEO));
    char *previous = SDL_GetClipboardText();
    std::string original = previous ? previous : ""; SDL_free(previous);
    std::atomic<bool> done{false};
    std::thread render([&] {
        auto *ctx = ImGui::CreateContext();
        ImGui::GetPlatformIO().Platform_GetClipboardTextFn = clipboard_get;
        ImGui::GetPlatformIO().Platform_SetClipboardTextFn = clipboard_set;
        ImGui::SetClipboardText("José 김 😀");
        assert(!strcmp(ImGui::GetClipboardText(), "José 김 😀"));
        ImGui::SetClipboardText("");
        assert(!*ImGui::GetClipboardText());
        auto &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 800); io.DeltaTime = 1.0f/60;
        unsigned char *pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        char name[96] = "José 김 😀";
        auto frame = [&](bool focus) {
            ImGui::NewFrame(); ImGui::Begin("Name");
            if (focus) ImGui::SetKeyboardFocusHere();
            ImGui::InputText("name", name, sizeof name);
            ImGui::End(); ImGui::Render();
        };
        frame(true); frame(false); frame(false);
        auto shortcut = [&](ImGuiKey key) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(key, true); frame(false);
            io.AddKeyEvent(key, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame(false);
        };
        shortcut(ImGuiKey_A); shortcut(ImGuiKey_C);
        assert(!strcmp(ImGui::GetClipboardText(), "José 김 😀"));
        shortcut(ImGuiKey_X); assert(!*name);
        shortcut(ImGuiKey_V); assert(!strcmp(name, "José 김 😀"));
        shortcut(ImGuiKey_Z); assert(!*name);
        ImGui::DestroyContext(ctx);
        done = true;
    });
    while (!done) { clipboard_service(false); SDL_Event event; while(SDL_PollEvent(&event)) {} SDL_Delay(1); }
    render.join();
    clipboard_service(false);
    char *system_text = SDL_GetClipboardText();
    assert(!strcmp(system_text, "José 김 😀")); SDL_free(system_text);
    assert(SDL_SetClipboardText("external ñ"));
    clipboard_service(true);
    assert(!strcmp(clipboard_get(nullptr), "external ñ"));
    // UI may already have stopped pumping; render callbacks must still terminate.
    std::thread closing([] { clipboard_set(nullptr, "closing"); assert(!strcmp(clipboard_get(nullptr), "closing")); });
    closing.join(); clipboard_service(false);
    SDL_SetClipboardText(original.c_str()); SDL_Quit();
}
