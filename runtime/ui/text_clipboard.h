#pragma once
#include <SDL3/SDL.h>
#include <string>
#include <mutex>
#include "imgui.h"
// UI owns SDL calls; render callbacks only exchange snapshots and never wait for UI.
static std::mutex clipboard_lock;
static std::string clipboard_cache, clipboard_pending;
static bool clipboard_write_pending, clipboard_refresh_pending = true;
static void clipboard_service(bool refresh) {
    std::string write; bool have_write;
    {
        std::lock_guard<std::mutex> guard(clipboard_lock);
        have_write = clipboard_write_pending;
        if (have_write) write.swap(clipboard_pending);
        clipboard_write_pending = false;
        refresh = refresh || clipboard_refresh_pending || have_write;
        clipboard_refresh_pending = false;
    }
    if (have_write) SDL_SetClipboardText(write.c_str());
    if (refresh) {
        char *value = SDL_GetClipboardText();
        std::lock_guard<std::mutex> guard(clipboard_lock);
        // A newer render-thread copy takes precedence over an older UI refresh.
        if (!clipboard_write_pending) clipboard_cache = value ? value : "";
        SDL_free(value);
    }
}
static const char *clipboard_get(ImGuiContext *) {
    static std::string text;
    std::lock_guard<std::mutex> guard(clipboard_lock);
    text = clipboard_cache; clipboard_refresh_pending = true;
    return text.c_str();
}
static void clipboard_set(ImGuiContext *, const char *text) {
    std::lock_guard<std::mutex> guard(clipboard_lock);
    clipboard_cache = clipboard_pending = text ? text : "";
    clipboard_write_pending = true;
}
