#include <assert.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <threads.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib.h>
#include <glibconfig.h>
#include <gtk/gtk.h>

#include "cJSON.h"
#include "khashl.h"

#include "waybar_cffi_module.h"

const size_t wbcffi_version = 2;
gint ICON_SIZE = 14;

typedef struct {
    wbcffi_module* waybar_module;
    GtkBox* container;
    mtx_t local_lock;
    int quit;
    thrd_t* thread;
} Instance;

typedef struct {
    uint64_t id;
    uint64_t workspace_id;
    char* app_id;
    char* title;
    uint64_t pos_x;
    uint64_t pos_y;
} Window;

KHASHL_MAP_INIT(
    KH_LOCAL,
    Windows,
    Windows,
    uint64_t,
    Window,
    kh_hash_uint64,
    kh_eq_generic);

typedef struct {
    int* ids;
    char** outputs;
    size_t len;
} WorkspaceOutputs;

mtx_t global_lock;
cnd_t global_update_cond;
int64_t stepping_counter = 0;
Windows* windows;
WorkspaceOutputs workspace_outputs;
int64_t current_focused_workspace = -1;
int64_t current_focused_window = -1;
thrd_t* ipc_thread = NULL;
int ipc_kill = 0;

int instance_count = 0;

#ifdef LOG_INFO
#define log_info(...) fprintf(stderr, __VA_ARGS__)
#else
#define log_info(...) (0)
#endif

int compare_window(const void* l, const void* r) {
    const Window* wl = l;
    const Window* wr = r;

    if (wl->pos_y != wr->pos_y) {
        return wl->pos_y - wr->pos_x;
    }
    return wl->pos_x - wr->pos_x;
}

int connect_to_niri() {
    const char* socket_path = getenv("NIRI_SOCKET");
    if (!socket_path) {
        log_info("[Niri Workspace Windows] Niri not running\n");
        return -1;
    }

    struct sockaddr_un addr;
    int socketfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketfd == -1) {
        log_info("[Niri Workspace Windows] Failed to create socket\n");
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = 0;

    if (connect(socketfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(socketfd);
        log_info("[Niri Workspace Windows] Failed to connect socket\n");
        return -1;
    }

    return socketfd;
}

int parse_ipc(const char* r, gsize len) {
    cJSON* root = cJSON_ParseWithLength(r, len);
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) == 0) {
        log_info("[Niri Workspace Windows] Empty response?\n");
        cJSON_Delete(root);
        return 0;
    }
    cJSON* ev = root->child;

    int has_event = 0;

    if (!strcmp(ev->string, "WorkspacesChanged")) {
        log_info("[Niri Workspace Windows] WorkspaceChanged\n");
        ev = ev->child;
        if (!cJSON_IsArray(ev)) {
            log_info("[Niri Workspace Windows] Workspaces not array?\n");
            cJSON_Delete(root);
            return 0;
        }

        for (size_t i = 0; i < workspace_outputs.len; i++) {
            free(workspace_outputs.outputs[i]);
        }

        workspace_outputs.len = cJSON_GetArraySize(ev);
        workspace_outputs.ids = realloc(
            workspace_outputs.ids,
            workspace_outputs.len * sizeof(*workspace_outputs.ids));
        workspace_outputs.outputs = realloc(
            workspace_outputs.outputs,
            workspace_outputs.len * sizeof(*workspace_outputs.outputs));

        cJSON* workspace = NULL;
        size_t i = 0;
        cJSON_ArrayForEach(workspace, ev) {
            int id =
                cJSON_GetObjectItemCaseSensitive(workspace, "id")->valueint;
            int is_focused =
                cJSON_GetObjectItemCaseSensitive(workspace, "is_focused")
                    ->valueint;
            char* output =
                strdup(cJSON_GetObjectItemCaseSensitive(workspace, "output")
                           ->valuestring);
            if (is_focused) {
                current_focused_workspace = id;
            }
            workspace_outputs.ids[i] = id;
            workspace_outputs.outputs[i] = output;
            i += 1;
        }

        has_event = 1;
    } else if (!strcmp(ev->string, "WindowsChanged")) {
        log_info("[Niri Workspace Windows] WindowsChanged\n");
        ev = ev->child;
        const cJSON* w = NULL;
        cJSON_ArrayForEach(w, ev) {
            int64_t id = cJSON_GetObjectItemCaseSensitive(w, "id")->valueint;
            int is_focused =
                cJSON_GetObjectItemCaseSensitive(w, "is_focused")->valueint;
            int64_t workspace_id =
                cJSON_GetObjectItemCaseSensitive(w, "workspace_id")->valueint;
            char* app_id = strdup(
                cJSON_GetObjectItemCaseSensitive(w, "app_id")->valuestring);
            char* title = strdup(
                cJSON_GetObjectItemCaseSensitive(w, "title")->valuestring);
            cJSON* pos = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(w, "layout"),
                "pos_in_scrolling_layout");
            uint64_t x = -1;
            uint64_t y = -1;
            if (cJSON_IsArray(pos)) {
                x = cJSON_GetArrayItem(pos, 0)->valueint;
                y = cJSON_GetArrayItem(pos, 1)->valueint;
            }

            if (is_focused && workspace_id == current_focused_workspace) {
                current_focused_window = id;
            }

            khint_t k;
            int absent;
            k = Windows_put(windows, id, &absent);
            if (!absent) {
                free(kh_val(windows, k).app_id);
                kh_val(windows, k).app_id = NULL;
            }
            kh_val(windows, k) = (Window){
                .id = id,
                .workspace_id = workspace_id,
                .app_id = app_id,
                .title = title,
                .pos_x = x,
                .pos_y = y,
            };
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WorkspaceActivated")) {
        log_info("[Niri Workspace Windows] WorkspaceActivated\n");
        if (cJSON_GetObjectItemCaseSensitive(ev, "focused")->valueint) {
            current_focused_workspace =
                cJSON_GetObjectItemCaseSensitive(ev, "id")->valueint;
            current_focused_window = -1;
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowFocusChanged")) {
        log_info("[Niri Workspace Windows] WindowFocusChanged\n");
        cJSON* id = cJSON_GetObjectItemCaseSensitive(ev, "id");
        if (cJSON_IsNull(id)) {
            current_focused_window = -1;
        } else {
            current_focused_window = id->valueint;
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowLayoutsChanged")) {
        log_info("[Niri Workspace Windows] WindowLayoutsChanged\n");
        ev = ev->child;
        cJSON* c;
        cJSON_ArrayForEach(c, ev) {
            int64_t id = cJSON_GetArrayItem(c, 0)->valueint;
            cJSON* d = cJSON_GetArrayItem(c, 1);
            cJSON* pos =
                cJSON_GetObjectItemCaseSensitive(d, "pos_in_scrolling_layout");
            uint64_t x = -1;
            uint64_t y = -1;
            if (cJSON_IsArray(pos)) {
                x = cJSON_GetArrayItem(pos, 0)->valueint;
                y = cJSON_GetArrayItem(pos, 1)->valueint;
            }

            khint_t k = Windows_get(windows, id);
            if (k == kh_end(windows)) {
                log_info(
                    "[Niri Workspace Windows] Changing unknown window layout: "
                    "%ld\n",
                    id);
            } else {
                kh_val(windows, k).pos_x = x;
                kh_val(windows, k).pos_y = y;
            }
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowOpenedOrChanged")) {
        log_info("[Niri Workspace Windows] WindowOpenedOrChanged\n");
        ev = ev->child;
        int64_t id = cJSON_GetObjectItemCaseSensitive(ev, "id")->valueint;
        int is_focused =
            cJSON_GetObjectItemCaseSensitive(ev, "is_focused")->valueint;
        int64_t workspace_id =
            cJSON_GetObjectItemCaseSensitive(ev, "workspace_id")->valueint;
        char* app_id =
            cJSON_GetObjectItemCaseSensitive(ev, "app_id")->valuestring;
        if (!app_id) {
            cJSON_Delete(root);
            return 0;
        }
        app_id = strdup(app_id);
        char* title =
            cJSON_GetObjectItemCaseSensitive(ev, "title")->valuestring;
        if (!title) {
            cJSON_Delete(root);
            return 0;
        }
        title = strdup(title);
        cJSON* pos = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(ev, "layout"),
            "pos_in_scrolling_layout");
        uint64_t x = -1;
        uint64_t y = -1;
        if (cJSON_IsArray(pos)) {
            x = cJSON_GetArrayItem(pos, 0)->valueint;
            y = cJSON_GetArrayItem(pos, 1)->valueint;
        }

        if (is_focused && workspace_id == current_focused_workspace) {
            current_focused_window = id;
        }

        khint_t k;
        int absent;
        k = Windows_put(windows, id, &absent);
        if (!absent) {
            free(kh_val(windows, k).app_id);
            kh_val(windows, k).app_id = NULL;
        }
        kh_val(windows, k) = (Window){
            .id = id,
            .workspace_id = workspace_id,
            .app_id = app_id,
            .title = title,
            .pos_x = x,
            .pos_y = y,
        };
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowClosed")) {
        log_info("[Niri Workspace Windows] WindowClosed\n");
        int64_t id = ev->child->valueint;
        khint_t k = Windows_get(windows, id);
        if (k != kh_end(windows)) {
            free(kh_val(windows, k).app_id);
            free(kh_val(windows, k).title);
            kh_val(windows, k).app_id = NULL;
            kh_val(windows, k).title = NULL;
            Windows_del(windows, k);
        }
        if (current_focused_window == id) {
            current_focused_window = -1;
        }
        has_event = 1;
    } else {
        log_info("[Niri Workspace Windows] Unhandled: %s\n", ev->string);
    }

    cJSON_Delete(root);
    return has_event;
}

struct ipc_arg {
    int socketfd;
    wbcffi_module* waybar_module;
    void (*queue_update)(wbcffi_module*);
};

int ipc(void* arg) {
    struct ipc_arg* ipc_arg = arg;
    int socketfd = ipc_arg->socketfd;
    wbcffi_module* m = ipc_arg->waybar_module;
    void (*queue_update)(wbcffi_module*) = ipc_arg->queue_update;

    GInputStream* unix_istream = g_unix_input_stream_new(socketfd, TRUE);
    GOutputStream* unix_ostream = g_unix_output_stream_new(socketfd, FALSE);
    GDataInputStream* istream = g_data_input_stream_new(unix_istream);
    GDataOutputStream* ostream = g_data_output_stream_new(unix_ostream);

    GError* e;
    if (!g_data_output_stream_put_string(
            ostream, "\"EventStream\"\n", NULL, &e)) {
        log_info(
            "[Niri Workspace Windows] Cannot write to socket: %s\n",
            e->message);
        return 1;
    }

    gsize len = 0;
    char* r = g_data_input_stream_read_line(istream, &len, NULL, &e);
    if (!r) {
        log_info(
            "[Niri Workspace Windows] Cannot read from socket: %s\n",
            e->message);
        return 1;
    }
    if (strcmp(r, "{\"Ok\":\"Handled\"}")) {
        log_info("[Niri Workspace Windows] Failed to start event stream\n");
        return 1;
    }

    windows = Windows_init();
    while ((r = g_data_input_stream_read_line(istream, &len, NULL, &e))) {
        mtx_lock(&global_lock);
        if (parse_ipc(r, len)) {
            stepping_counter += 1;
            cnd_broadcast(&global_update_cond);
        }
        // if (ipc_kill) {
        //     g_free(r);
        //     return 0;
        // }
        mtx_unlock(&global_lock);
        g_free(r);
    }

    free(ipc_arg);
    return 0;
}

thrd_t* spawn_main_instance_thread(
    int socketfd,
    wbcffi_module* m,
    void (*queue_update)(wbcffi_module*)) {
    mtx_init(&global_lock, mtx_plain);
    thrd_t* thread = malloc(sizeof(*thread));
    struct ipc_arg* ipc_arg = malloc(sizeof(*ipc_arg));
    *ipc_arg = (struct ipc_arg){
        .socketfd = socketfd,
        .waybar_module = m,
        .queue_update = queue_update,
    };
    thrd_create(thread, ipc, ipc_arg);
    return thread;
}

struct sub_instance_thread_arg {
    Instance* inst;
    void (*queue_update)(wbcffi_module*);
};

int sub_instance_thread(void* a) {
    struct sub_instance_thread_arg* arg = a;
    int64_t local_stepping_counter = -1;
    while (1) {
        mtx_lock(&arg->inst->local_lock);
        if (arg->inst->quit) {
            mtx_unlock(&arg->inst->local_lock);
            return 0;
        }
        mtx_unlock(&arg->inst->local_lock);

        mtx_lock(&global_lock);
        while (stepping_counter == local_stepping_counter) {
            cnd_wait(&global_update_cond, &global_lock);
        }
        arg->queue_update(arg->inst->waybar_module);
        local_stepping_counter = stepping_counter;
        mtx_unlock(&global_lock);
    }
    return 0;
}

thrd_t* spawn_sub_instance_thread(
    Instance* inst,
    void (*queue_update)(wbcffi_module*)) {
    thrd_t* thread = malloc(sizeof(*thread));
    struct sub_instance_thread_arg* arg = malloc(sizeof(*arg));
    *arg = (struct sub_instance_thread_arg){
        .inst = inst,
        .queue_update = queue_update,
    };
    thrd_create(thread, sub_instance_thread, arg);
    return thread;
}

void* wbcffi_init(
    const wbcffi_init_info* init_info,
    const wbcffi_config_entry* config_entries,
    size_t config_entries_len) {
    instance_count += 1;
    for (size_t i = 0; i < config_entries_len; i++) {
        const char* key = config_entries[i].key;
        const char* value = config_entries[i].value;
        if (!strcmp(key, "icon_size")) {
            char* endptr = NULL;
            errno = 0;
            gint icon_size = strtol(value, &endptr, 10);
            if (errno || *endptr != '\n') {
                log_info(
                    "[Niri Workspace Windows] \"icon_size\" must be a "
                    "number\n");
                return NULL;
            }
            ICON_SIZE = icon_size;
        }
    }

    Instance* inst = malloc(sizeof(Instance));
    inst->waybar_module = init_info->obj;

    log_info("Instance count %d", instance_count);
    mtx_init(&inst->local_lock, mtx_plain);
    inst->quit = 0;

    if (ipc_thread == NULL) {
        int socketfd = connect_to_niri();
        if (socketfd == -1) {
            return NULL;
        }

        ipc_thread = spawn_main_instance_thread(
            socketfd, inst->waybar_module, init_info->queue_update);
        cnd_init(&global_update_cond);
    }
    inst->thread = spawn_sub_instance_thread(inst, init_info->queue_update);

    GtkContainer* root = init_info->get_root_widget(init_info->obj);

    inst->container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_widget_set_name(GTK_WIDGET(inst->container), "workspace-windows");
    gtk_container_add(GTK_CONTAINER(root), GTK_WIDGET(inst->container));

    return inst;
}

void clean_global_resource() {
    mtx_lock(&global_lock);
    ipc_kill = 1;
    mtx_unlock(&global_lock);

    thrd_join(*ipc_thread, NULL);
    free(ipc_thread);
    ipc_thread = NULL;
    cnd_destroy(&global_update_cond);
    mtx_destroy(&global_lock);
    ipc_kill = 0;

    khint_t k;
    kh_foreach(windows, k) {
        free(kh_val(windows, k).app_id);
        free(kh_val(windows, k).title);
    }
    Windows_destroy(windows);

    for (size_t i = 0; i < workspace_outputs.len; i++) {
        free(workspace_outputs.outputs[i]);
    }
    free(workspace_outputs.ids);
    free(workspace_outputs.outputs);
    workspace_outputs.ids = NULL;
    workspace_outputs.outputs = NULL;
    workspace_outputs.len = 0;
    current_focused_workspace = -1;
    current_focused_window = -1;
}

void wbcffi_deinit(void* i) {
    Instance* inst = i;

    mtx_lock(&inst->local_lock);
    inst->quit = 1;
    mtx_unlock(&inst->local_lock);

    mtx_lock(&global_lock);
    cnd_broadcast(&global_update_cond);
    stepping_counter += 1;
    mtx_unlock(&global_lock);

    thrd_join(*inst->thread, NULL);
    free(inst->thread);
    mtx_destroy(&inst->local_lock);
    free(inst);

    instance_count -= 1;

    if (instance_count == 0) {
        clean_global_resource();
    }
}

GArray* get_search_prefixes() {
    GArray* prefixes = g_array_new(FALSE, TRUE, sizeof(GString*));
    GString* str = g_string_new(getenv("HOME"));
    g_string_append(str, "/.local/share/");
    g_array_append_val(prefixes, str);
    str = g_string_new("/usr/share/");
    g_array_append_val(prefixes, str);
    str = g_string_new("/usr/local/share/");
    g_array_append_val(prefixes, str);

    return prefixes;
}

GtkWidget* widget_from_app_id(const char* app_id) {
    GArray* search_prefixes = get_search_prefixes();
    char* app_folders[] = {"", "applications/"};
    char* suffixes[] = {"", ".desktop"};
    const char* app_icon = NULL;
    for (int i = 0; i < search_prefixes->len; i++) {
        for (int j = 0; j < sizeof(app_folders) / sizeof(*app_folders); j++) {
            for (int k = 0; k < sizeof(suffixes) / sizeof(*suffixes); k++) {
                GString* str = g_string_new("");
                g_string_append(
                    str, g_array_index(search_prefixes, GString*, i)->str);
                g_string_append(str, app_folders[j]);
                g_string_append(str, app_id);
                g_string_append(str, suffixes[k]);
                GDesktopAppInfo* app_info =
                    g_desktop_app_info_new_from_filename(str->str);
                g_string_free(str, TRUE);
                if (app_info) {
                    const char* s =
                        g_desktop_app_info_get_string(app_info, "Icon");
                    if (s) {
                        app_icon = s;
                        goto end_of_desktop_file_seaching;
                    }
                }
            }
        }
    }
end_of_desktop_file_seaching:;
    for (int i = 0; i < search_prefixes->len; i++) {
        g_string_free(g_array_index(search_prefixes, GString*, i), TRUE);
    }
    g_array_free(search_prefixes, FALSE);

    GtkImage* img = GTK_IMAGE(gtk_image_new());
    if (!app_icon) {
        gtk_image_set_from_icon_name(
            img, "image-missing", GTK_ICON_SIZE_INVALID);
    } else if (app_icon[0] == '/') {
        GError* e;
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(app_icon, &e);
        if (!pixbuf) {
            log_info("%s", e->message);
            gtk_widget_set_visible(GTK_WIDGET(img), FALSE);
            goto set_button_image;
        }
        cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(
            pixbuf,
            gtk_widget_get_scale_factor(GTK_WIDGET(img)),
            gtk_widget_get_window(GTK_WIDGET(img)));
        gtk_image_set_from_surface(img, surface);
    } else {
        gtk_image_set_from_icon_name(img, app_icon, GTK_ICON_SIZE_INVALID);
    }

    gtk_image_set_pixel_size(img, ICON_SIZE);
set_button_image:;
    GtkButton* btn = GTK_BUTTON(gtk_button_new());
    gtk_button_set_image(btn, GTK_WIDGET(img));

    return GTK_WIDGET(btn);
}

char* get_current_output(Instance* instance) {
    GdkWindow* gdk_win = gtk_widget_get_window(GTK_WIDGET(instance->container));
    if (!gdk_win) {
        return NULL;
    }
    GdkScreen* gdk_screen = gdk_window_get_screen(gdk_win);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    int monitor = gdk_screen_get_monitor_at_window(gdk_screen, gdk_win);
    return gdk_screen_get_monitor_plug_name(gdk_screen, monitor);
#pragma GCC diagnostic pop
}

int should_display_window_icon(
    Window window,
    uint64_t current_focused_worksapce,
    const gchar* current_output) {
    uint64_t workspace_id = window.workspace_id;
    int focused_workspace_on_output = 0;
    for (size_t i = 0; i < workspace_outputs.len; i++) {
        if (current_focused_workspace == workspace_outputs.ids[i] &&
            !strcmp(current_output, workspace_outputs.outputs[i])) {
            focused_workspace_on_output = 1;
            break;
        }
    }

    if (focused_workspace_on_output) {
        return workspace_id == current_focused_workspace;
    }
    return 0;
}

// Just remove all elements and repopulate container contents
void wbcffi_update(void* inst) {
    mtx_lock(&global_lock);
    if (current_focused_workspace == -1) {
        mtx_unlock(&global_lock);
        return;
    }
    Instance* instance = inst;

    char* current_output = get_current_output(instance);

    khint_t k;
    size_t dw_n = 0;
    if (current_focused_window >= 0) {
        kh_foreach(windows, k) {
            dw_n += should_display_window_icon(
                kh_val(windows, k), current_focused_workspace, current_output);
        }
    }
    Window displaying_windows[dw_n];
    if (current_focused_window >= 0) {
        size_t i = 0;
        kh_foreach(windows, k) {
            if (should_display_window_icon(
                    kh_val(windows, k),
                    current_focused_workspace,
                    current_output)) {
                displaying_windows[i] = kh_val(windows, k);
                i += 1;
            }
        }
        qsort(
            displaying_windows,
            dw_n,
            sizeof(*displaying_windows),
            compare_window);
    }

    g_free(current_output);

    GList* children =
        gtk_container_get_children(GTK_CONTAINER(instance->container));
    while (children) {
        gtk_container_remove(
            GTK_CONTAINER(instance->container), children->data);
        children = children->next;
    }

    for (size_t i = 0; i < dw_n; i++) {
        GtkWidget* widget = widget_from_app_id(displaying_windows[i].app_id);
        gtk_widget_set_tooltip_text(widget, displaying_windows[i].title);
        GtkStyleContext* context = gtk_widget_get_style_context(widget);
        if (displaying_windows[i].id == current_focused_window) {
            gtk_style_context_add_class(context, "focused");
        } else {
            gtk_style_context_remove_class(context, "focused");
        }
        gtk_box_pack_start(instance->container, widget, FALSE, FALSE, 0);
        gtk_box_reorder_child(instance->container, widget, i);
        gtk_widget_show_all(GTK_WIDGET(instance->container));
    }
    mtx_unlock(&global_lock);
}

void wbcffi_refresh(void* instance, int signal) {
    // What does this do?
}

void wbcffi_doaction(void* instance, const char* name) {
    // No actions supported
}
