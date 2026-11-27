/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui.h"
#include <stdlib.h>

/* A touch target or drag sample fits in one atomic word. The high word
 * identifies the current page. The low word carries coordinates and the target. */
enum { X_BITS = 11, Y_BITS = 10, INDEX_SHIFT = X_BITS + Y_BITS,
    X_MASK = (1 << X_BITS) - 1, Y_MASK = (1 << Y_BITS) - 1,
    PIXEL_MASK = (1 << INDEX_SHIFT) - 1, HIT_RADIUS = 22, DRAG_SLOP = 5,
    RAIL_TARGET = SP_ARC_POINTS + 1 };

static unsigned pixel(int x, int y)
{
    unsigned px = (unsigned)sp_clamp(x, 0, X_MASK);
    unsigned py = (unsigned)sp_clamp(y, 0, Y_MASK);
    return px | py << X_BITS;
}

void sp_reset_drag(SpRuntime *r)
{
    ++r->drag_generation;
    r->surface_contact = 0;
    atomic_store(&r->drag_rail, 0);
    for (int i = 0; i < SP_ARC_POINTS; ++i)
        atomic_store(&r->drag_handles[i], 0);
}

void sp_publish_anchor(SpRuntime *r, int index, int x, int y)
{
    uint64_t handle = (uint64_t)r->drag_generation << 32 |
        (uint64_t)(index + 1) << INDEX_SHIFT | pixel(x, y);
    atomic_store(&r->drag_handles[index], handle);
}

void sp_publish_rail(SpRuntime *r, az_rect rail)
{
    r->chart_rail = rail;
    uint64_t geometry = (uint64_t)r->drag_generation << 32 |
        (uint64_t)rail.w << INDEX_SHIFT | pixel(rail.x, rail.y);
    atomic_store(&r->drag_rail, geometry);
}

static uint64_t rail_hit(const SpRuntime *r, int x, int y)
{
    uint64_t packed = atomic_load(&r->drag_rail);
    az_rect box = {(int)(packed & X_MASK), (int)(packed >> X_BITS & Y_MASK),
        (int)((uint32_t)packed >> INDEX_SHIFT), SP_RAIL_HEIGHT};
    if (!sp_contains(box, x, y))
        return 0;
    return (packed >> 32 << 32) | (uint64_t)RAIL_TARGET << INDEX_SHIFT | pixel(x, y);
}

static bool rail_target(uint64_t sample)
{
    return (sample >> INDEX_SHIFT & 7) == RAIL_TARGET;
}

static uint64_t first_handle(const SpRuntime *r, int x, int y)
{
    if (atomic_load(&r->phase) != SP_IDLE)
        return 0;
    uint64_t hit = 0;
    int nearest = HIT_RADIUS * HIT_RADIUS + 1;
    for (int i = 0; i < SP_ARC_POINTS; ++i) {
        uint64_t p = atomic_load(&r->drag_handles[i]);
        int dx = x - (int)(p & X_MASK), dy = y - (int)(p >> X_BITS & Y_MASK);
        int distance = dx * dx + dy * dy;
        if (p && distance < nearest) {
            nearest = distance;
            hit = p;
        }
    }
    return hit ? hit : rail_hit(r, x, y);
}

static bool dragged(int dx, int dy)
{
    return abs(dx) > DRAG_SLOP || abs(dy) > DRAG_SLOP;
}

static uint64_t drag_sample(uint64_t handle, int x, int y, int dx, int dy)
{
    int center_x = (int)(handle & X_MASK), center_y = (int)(handle >> X_BITS & Y_MASK);
    return (handle & ~(uint64_t)PIXEL_MASK) | pixel(center_x + x - dx, center_y + y - dy);
}

static int begin_touch(SpRuntime *r, const az_touch *touch)
{
    if (!touch->down)
        return AZ_TOUCH_PASS;
    r->drag_contact = first_handle(r, touch->x, touch->y);
    r->drag_x = touch->x;
    r->drag_y = touch->y;
    r->drag_started = false;
    if (rail_target(r->drag_contact))
        atomic_store(&r->drag_event, r->drag_contact);
    return r->drag_contact ? AZ_TOUCH_TAKE : AZ_TOUCH_PASS;
}

int sp_touch_drag(const az_touch *touch, void *user)
{
    SpRuntime *r = user;
    if (!r->drag_contact)
        return begin_touch(r, touch);
    r->drag_started |= dragged(touch->x - r->drag_x, touch->y - r->drag_y);
    if (r->drag_started)
        atomic_store(&r->drag_event, drag_sample(r->drag_contact, touch->x, touch->y, r->drag_x, r->drag_y));
    if (!touch->down)
        r->drag_contact = 0;
    return AZ_TOUCH_TAKE;
}

static void edit_anchor(SpRuntime *r, uint64_t sample)
{
    az_rect plot = r->arc_plot;
    SpArc arc = *sp_arc(&r->options);
    int index = (int)(sample >> INDEX_SHIFT & 7) - 1;
    SpAnchor *point = &arc.points[index];
    double position = ((int)(sample & X_MASK) - plot.x) / (double)plot.w;
    double energy = 10.0 - 9.0 * ((int)(sample >> X_BITS & Y_MASK) - plot.y) / plot.h;
    if (index > 0 && index < arc.count - 1)
        point->position = sp_clamp(position, arc.points[index - 1].position + 0.08,
                                  arc.points[index + 1].position - 0.08);
    point->energy = sp_clamp(energy, 1, 10);
    r->options.custom_arc = arc;
}

static void apply_sample(SpRuntime *r, uint64_t sample)
{
    if (rail_target(sample)) {
        int x = (int)(sample & X_MASK) - r->chart_rail.x;
        sp_chart_scroll_to(r, (double)x / (r->chart_rail.w - 1));
    } else {
        edit_anchor(r, sample);
    }
}

bool sp_poll_drag(SpRuntime *r)
{
    uint64_t sample = atomic_exchange(&r->drag_event, 0);
    if (!sample || (uint32_t)(sample >> 32) != r->drag_generation)
        return false;
    apply_sample(r, sample);
    return true;
}

bool sp_surface_drag(SpRuntime *r, const az_event *event)
{
    if (event->kind == AZ_EV_DOWN) {
        r->surface_contact = first_handle(r, event->x, event->y);
        r->surface_x = event->x;
        r->surface_y = event->y;
        if (rail_target(r->surface_contact))
            apply_sample(r, r->surface_contact);
        return r->surface_contact != 0;
    }
    if (event->kind != AZ_EV_UP || !r->surface_contact)
        return false;
    if (dragged(event->x - r->surface_x, event->y - r->surface_y))
        apply_sample(r, drag_sample(r->surface_contact, event->x, event->y, r->surface_x, r->surface_y));
    r->surface_contact = 0;
    return true;
}
