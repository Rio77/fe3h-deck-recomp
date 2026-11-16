// Content written before a render pass must survive its implicit first-use clear.
// Track mip levels; conservatively preserve other layers of a written mip too.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint64_t frame; uint32_t levels; } VkEffectContent;
static inline void vk_effect_content_write(VkEffectContent *s, uint64_t frame, uint32_t level) {
    if (s->frame != frame) { s->frame = frame; s->levels = 0; }
    s->levels |= level < 32 ? UINT32_C(1) << level : UINT32_MAX;
}
static inline bool vk_effect_content_written(const VkEffectContent *s, uint64_t frame, uint32_t level) {
    return s->frame == frame && (level < 32 ? (s->levels & (UINT32_C(1) << level)) != 0 : s->levels != 0);
}
