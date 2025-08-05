#include <furi.h>
#include <gui/gui.h>
#include <dialogs/dialogs.h>
#include <input/input.h>
#include <stdlib.h>
#include <furi_hal.h>
#include <furi_hal_gpio.h>
#include <furi_hal_spi.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_resources.h>
#include <nrf24.h>
#include <notification/notification_messages.h>
#include <dolphin/dolphin.h>
#include "nrf24_jammer_icons.h"
#include "gui/elements.h"
#include <toolbox/stream/file_stream.h>
#include <dialogs/dialogs.h>

#include <stringp.h>

#define TAG "jammer"

#define MARGIN_LEFT  5
#define MARGIN_TOP   5
#define MARGIN_BOT   5
#define MARGIN_RIGHT 5
#define KEY_WIDTH    5
#define KEY_HEIGHT   10
#define KEY_PADDING  2

typedef struct {
    FuriMutex* mutex;
    bool is_thread_running;
    bool is_nrf24_connected;
    bool close_thread_please;
    uint8_t jam_type; // 0:bluetooth, 1:WiFi, 2:full
    FuriThread* jam_thread;
} PluginState;

typedef enum {
    EventTypeTick,
    EventTypeKey,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;

// Hoping for different technologies
uint8_t hopping_channels_0[79];
uint8_t hopping_channels_1[] = {12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62, 67, 72};
uint8_t hopping_channels_2[128];
uint8_t hopping_channels_len[] = {79, 13, 128, 0};

char* jam_types[] = {"bluetooth", "WiFi", "full"};
uint8_t* hopping_channels;

static void render_callback(Canvas* const canvas, void* ctx) {
    furi_assert(ctx);
    PluginState* plugin_state = ctx;
    //furi_mutex_acquire(plugin_state->mutex, FuriWaitForever);

    // border around the edge of the screen
    canvas_draw_frame(canvas, 0, 0, 128, 64);

    canvas_set_font(canvas, FontSecondary);

    char tmp[128];
    snprintf(tmp, 128, "^ type:%s", jam_types[plugin_state->jam_type]);
    canvas_draw_str_aligned(canvas, 10, 3, AlignLeft, AlignTop, tmp);

    if(!plugin_state->is_thread_running) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 10, 20, AlignLeft, AlignBottom, "Press Ok button to start");

        if(!plugin_state->is_nrf24_connected) {
            canvas_draw_str_aligned(
                canvas, 10, 30, AlignLeft, AlignBottom, "Connect NRF24 to GPIO!");
        }
    } else if(plugin_state->is_thread_running) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 3, 30, AlignLeft, AlignBottom, "Causing mayhem...");
        canvas_draw_str_aligned(canvas, 3, 40, AlignLeft, AlignBottom, "Please wait!");
        canvas_draw_str_aligned(canvas, 3, 50, AlignLeft, AlignBottom, "Press back to exit.");
    } else {
        canvas_draw_str_aligned(canvas, 3, 10, AlignLeft, AlignBottom, "Unknown Error");
        canvas_draw_str_aligned(canvas, 3, 20, AlignLeft, AlignBottom, "press back");
        canvas_draw_str_aligned(canvas, 3, 30, AlignLeft, AlignBottom, "to exit");
    }
    uint8_t limit = hopping_channels_len[plugin_state->jam_type];
    canvas_draw_frame(canvas, 0, 52, 128, 13);
    if(limit > 0) {
        for(int i = 0; i < limit; i++) {
            canvas_draw_line(canvas, hopping_channels[i] + 1, 53, hopping_channels[i] + 1, 64);
        }
    }
    //furi_mutex_release(plugin_state->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    PluginEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, &event, 100);
}

static void jammer_state_init(PluginState* const plugin_state) {
    plugin_state->is_thread_running = false;
}

// entrypoint for worker
static int32_t mj_worker_thread(void* ctx) {
    PluginState* plugin_state = ctx;
    plugin_state->is_thread_running = true;

#define size 32

    FURI_LOG_D(TAG, "Starting optimized carrier jamming");

    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_blink_red_100);

    nrf24_set_tx_mode(nrf24_HANDLE);
    nrf24_startConstCarrier(nrf24_HANDLE, 3, 72);

    uint8_t current_channel = 0;
    uint8_t limit = hopping_channels_len[plugin_state->jam_type];

    while(!plugin_state->close_thread_please) {
        for(int ch = 0; ch < limit && !plugin_state->close_thread_please; ch++) {
            current_channel = hopping_channels[ch];
            nrf24_write_reg(nrf24_HANDLE, REG_RF_CH, current_channel);
        }
    }

    nrf24_stopConstCarrier(nrf24_HANDLE);

    furi_record_close(RECORD_NOTIFICATION);
    plugin_state->is_thread_running = false;
    return 0;
}

int32_t jammer_app(void* p) {
    UNUSED(p);
    if(!furi_hal_power_is_otg_enabled()) furi_hal_power_enable_otg();
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(PluginEvent));
    dolphin_deed(DolphinDeedPluginStart);
    PluginState* plugin_state = malloc(sizeof(PluginState));
    jammer_state_init(plugin_state);
    plugin_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!plugin_state->mutex) {
        FURI_LOG_E("jammer", "cannot create mutex\r\n");
        furi_message_queue_free(event_queue);
        free(plugin_state);
        return 255;
    }

    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);

    // Set system callbacks
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, plugin_state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    plugin_state->jam_thread = furi_thread_alloc();
    furi_thread_set_name(plugin_state->jam_thread, "Jammer Worker");
    furi_thread_set_stack_size(plugin_state->jam_thread, 2048);
    furi_thread_set_context(plugin_state->jam_thread, plugin_state);
    furi_thread_set_callback(plugin_state->jam_thread, mj_worker_thread);
    FURI_LOG_D(TAG, "nrf24 init...");
    nrf24_init();
    FURI_LOG_D(TAG, "nrf24 init done!");
    PluginEvent event;
    for(int i = 0; i < 79; i++) hopping_channels_0[i] = i + 2;
    for(int i = 0; i < 128; i++) hopping_channels_2[i] = i * 2;
    hopping_channels = hopping_channels_0;
    plugin_state->is_nrf24_connected = true;
    if(!nrf24_check_connected(nrf24_HANDLE)) {
        plugin_state->is_nrf24_connected = false;
    }

    for(bool processing = true; processing;) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 100);
        if(event_status == FuriStatusOk) {
            // press events
            if(event.type == EventTypeKey) {
                if(event.input.type == InputTypePress) {
                    //furi_mutex_acquire(plugin_state->mutex, FuriWaitForever);

                    switch(event.input.key) {
                    case InputKeyUp:
                        if(!plugin_state->is_thread_running) {
                            plugin_state->jam_type = (plugin_state->jam_type + 1) % 3;

                            switch(plugin_state->jam_type) {
                            case 0:
                                hopping_channels = hopping_channels_0;
                                break;
                            case 1:
                                hopping_channels = hopping_channels_1;
                                break;
                            case 2:
                                hopping_channels = hopping_channels_2;
                                break;
                            default:
                                break;
                            }

                            view_port_update(view_port);
                        }
                        break;
                    case InputKeyDown:
                        if(!plugin_state->is_thread_running) {
                            plugin_state->jam_type =
                                (plugin_state->jam_type == 0) ? 2 : (plugin_state->jam_type - 1);

                            switch(plugin_state->jam_type) {
                            case 0:
                                hopping_channels = hopping_channels_0;
                                break;
                            case 1:
                                hopping_channels = hopping_channels_1;
                                break;
                            case 2:
                                hopping_channels = hopping_channels_2;
                                break;
                            default:
                                break;
                            }

                            view_port_update(view_port);
                        }
                        break;
                    case InputKeyOk:
                        if(!plugin_state->is_thread_running) {
                            if(!nrf24_check_connected(nrf24_HANDLE) ||
                               !plugin_state->is_nrf24_connected) {
                                plugin_state->is_nrf24_connected = false;
                                notification_message(notification, &sequence_error);
                            } else {
                                furi_thread_start(plugin_state->jam_thread);
                            }
                            view_port_update(view_port);
                        }

                        break;
                    case InputKeyBack:
                        FURI_LOG_D(TAG, "terminating thread...");
                        if(!plugin_state->is_thread_running) processing = false;

                        plugin_state->close_thread_please = true;

                        if(plugin_state->is_thread_running && plugin_state->jam_thread) {
                            plugin_state->is_thread_running = false;
                            furi_thread_join(
                                plugin_state->jam_thread); // wait until thread is finished
                            view_port_update(view_port);
                        }
                        plugin_state->close_thread_please = false;

                        break;
                    default:
                        break;
                    }

                    //view_port_update(view_port);
                }
            }
        }

        // furi_mutex_release(plugin_state->mutex);
    }

    furi_thread_free(plugin_state->jam_thread);
    FURI_LOG_D(TAG, "nrf24 deinit...");
    nrf24_deinit();
    furi_hal_power_disable_otg();
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    // furi_mutex_free(plugin_state->mutex);
    free(plugin_state);

    return 0;
}
