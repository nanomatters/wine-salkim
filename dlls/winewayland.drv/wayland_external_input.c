/*
 * Process-local external input support
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"

static pthread_mutex_t external_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t external_input_cond = PTHREAD_COND_INITIALIZER;
static struct wine_wayland_external_input_handler external_input_handler;
static unsigned int external_input_callbacks;
static unsigned int external_input_focus_serial;
static BOOL external_input_keyboard_focused;
int wayland_external_input_active;
int wayland_external_input_registered;

static int wayland_external_input_set_handler(
        const struct wine_wayland_external_input_handler *handler)
{
    struct wine_wayland_external_input_event focus_event;
    BOOL notify_focus = FALSE;
    BOOL deactivate = !handler;

    pthread_mutex_lock(&external_input_mutex);

    if (!handler)
    {
        __atomic_store_n(&wayland_external_input_registered, FALSE, __ATOMIC_RELEASE);
        __atomic_store_n(&wayland_external_input_active, FALSE, __ATOMIC_RELEASE);
        __atomic_store_n(&external_input_handler.event, NULL, __ATOMIC_RELEASE);
        while (external_input_callbacks)
            pthread_cond_wait(&external_input_cond, &external_input_mutex);
        external_input_handler.context = NULL;
    }
    else if (handler->size < sizeof(*handler) ||
             handler->version != WINE_WAYLAND_EXTERNAL_INPUT_VERSION ||
             !handler->event || external_input_handler.event)
    {
        pthread_mutex_unlock(&external_input_mutex);
        return 0;
    }
    else
    {
        external_input_handler.context = handler->context;
        external_input_handler.size = handler->size;
        external_input_handler.version = handler->version;
        __atomic_store_n(&external_input_handler.event, handler->event, __ATOMIC_RELEASE);
        __atomic_store_n(&wayland_external_input_registered, TRUE, __ATOMIC_RELEASE);
        focus_event = (struct wine_wayland_external_input_event)
        {
            .size = sizeof(focus_event),
            .type = WINE_WAYLAND_EXTERNAL_INPUT_FOCUS,
            .code = external_input_focus_serial,
            .state = external_input_keyboard_focused,
        };
        notify_focus = TRUE;
    }

    pthread_mutex_unlock(&external_input_mutex);
    if (notify_focus) wayland_external_input_emit(&focus_event);
    if (deactivate) wayland_pointer_set_external_input_active(FALSE);
    return 1;
}

static int wayland_external_input_set_active(int active)
{
    BOOL registered;

    pthread_mutex_lock(&external_input_mutex);
    registered = external_input_handler.event != NULL;
    pthread_mutex_unlock(&external_input_mutex);
    if (!registered) return 0;

    active = active != 0;
    if (!active) __atomic_store_n(&wayland_external_input_active, FALSE, __ATOMIC_RELEASE);
    wayland_pointer_set_external_input_active(active);
    if (active) __atomic_store_n(&wayland_external_input_active, TRUE, __ATOMIC_RELEASE);
    return 1;
}

BOOL wayland_external_input_emit(const struct wine_wayland_external_input_event *event)
{
    int (*callback)(void *, const struct wine_wayland_external_input_event *);
    void *context;
    int consumed;

    if (!wayland_external_input_is_registered()) return FALSE;

    pthread_mutex_lock(&external_input_mutex);
    if (!(callback = external_input_handler.event))
    {
        pthread_mutex_unlock(&external_input_mutex);
        return FALSE;
    }
    context = external_input_handler.context;
    external_input_callbacks++;
    pthread_mutex_unlock(&external_input_mutex);

    consumed = callback(context, event);

    pthread_mutex_lock(&external_input_mutex);
    if (!--external_input_callbacks) pthread_cond_broadcast(&external_input_cond);
    pthread_mutex_unlock(&external_input_mutex);
    return consumed != 0;
}

void wayland_external_input_set_keyboard_focus(BOOL focused)
{
    struct wine_wayland_external_input_event event;

    pthread_mutex_lock(&external_input_mutex);
    focused = focused != FALSE;
    if (external_input_keyboard_focused == focused)
    {
        pthread_mutex_unlock(&external_input_mutex);
        return;
    }
    external_input_keyboard_focused = focused;
    event = (struct wine_wayland_external_input_event)
    {
        .size = sizeof(event),
        .type = WINE_WAYLAND_EXTERNAL_INPUT_FOCUS,
        .code = ++external_input_focus_serial,
        .state = focused,
    };
    pthread_mutex_unlock(&external_input_mutex);

    wayland_external_input_emit(&event);
}

DECLSPEC_EXPORT const struct wine_wayland_external_input_api __wine_wayland_external_input_v1 =
{
    sizeof(struct wine_wayland_external_input_api),
    WINE_WAYLAND_EXTERNAL_INPUT_VERSION,
    wayland_external_input_set_handler,
    wayland_external_input_set_active,
};
