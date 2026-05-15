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
} WBObject;

// This static variable is shared between all instances of this module
static int instance_count = 0;

typedef struct {
    uint64_t id;
    uint64_t workspace_id;
    char* app_id;
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
mtx_t data_lock;
Windows* windows;
int64_t current_focused_workspace = -1;
int64_t current_focused_window = -1;

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
        fprintf(stderr, "[Niri Workspace Windows] Niri not running");
        return -1;
    }

    struct sockaddr_un addr;
    int socketfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketfd == -1) {
        fprintf(stderr, "[Niri Workspace Windows] Failed to create socket");
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = 0;

    if (connect(socketfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(socketfd);
        fprintf(stderr, "[Niri Workspace Windows] Failed to connect socket");
        return -1;
    }

    return socketfd;
}

int parse_ipc(
    const char* r,
    gsize len,
    wbcffi_module* m,
    void (*queue_update)(wbcffi_module*)) {
    const cJSON* ev = cJSON_ParseWithLength(r, len);
    if (!cJSON_IsObject(ev) || cJSON_GetArraySize(ev) == 0) {
        fprintf(stderr, "[Niri Workspace Windows] Empty response?");
        return 0;
    }
    mtx_lock(&data_lock);
    ev = ev->child;

    int has_event = 0;

    if (!strcmp(ev->string, "WorkspacesChanged")) {
        fprintf(stderr, "[Niri Workspace Windows] WorkspaceChanged\n");
        ev = ev->child;
        if (!cJSON_IsArray(ev)) {
            fprintf(stderr, "[Niri Workspace Windows] Workspaces not array?");
            mtx_unlock(&data_lock);
            return 0;
        }

        cJSON* workspace = NULL;
        cJSON_ArrayForEach(workspace, ev) {
            int id =
                cJSON_GetObjectItemCaseSensitive(workspace, "id")->valueint;
            int is_focused =
                cJSON_GetObjectItemCaseSensitive(workspace, "is_focused")
                    ->valueint;
            if (is_focused) {
                current_focused_workspace = id;
            }
        }

        has_event = 1;
    } else if (!strcmp(ev->string, "WindowsChanged")) {
        fprintf(stderr, "[Niri Workspace Windows] WindowsChanged\n");
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
                .pos_x = x,
                .pos_y = y,
            };
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WorkspaceActivated")) {
        fprintf(stderr, "[Niri Workspace Windows] WorkspaceActivated\n");
        if (cJSON_GetObjectItemCaseSensitive(ev, "focused")->valueint) {
            current_focused_workspace =
                cJSON_GetObjectItemCaseSensitive(ev, "id")->valueint;
            current_focused_window = -1;
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowFocusChanged")) {
        fprintf(stderr, "[Niri Workspace Windows] WindowFocusChanged\n");
        cJSON* id = cJSON_GetObjectItemCaseSensitive(ev, "id");
        if (cJSON_IsNull(id)) {
            current_focused_window = -1;
        } else {
            current_focused_window = id->valueint;
        }
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowLayoutsChanged")) {
        fprintf(stderr, "[Niri Workspace Windows] WindowLayoutsChanged\n");
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
                fprintf(
                    stderr,
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
        fprintf(stderr, "[Niri Workspace Windows] WindowOpenedOrChanged\n");
        ev = ev->child;
        int64_t id = cJSON_GetObjectItemCaseSensitive(ev, "id")->valueint;
        int is_focused =
            cJSON_GetObjectItemCaseSensitive(ev, "is_focused")->valueint;
        int64_t workspace_id =
            cJSON_GetObjectItemCaseSensitive(ev, "workspace_id")->valueint;
        char* app_id =
            cJSON_GetObjectItemCaseSensitive(ev, "app_id")->valuestring;
        if (!app_id) {
            mtx_unlock(&data_lock);
            return 0;
        }
        app_id = strdup(app_id);
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
            .pos_x = x,
            .pos_y = y,
        };
        has_event = 1;
    } else if (!strcmp(ev->string, "WindowClosed")) {
        fprintf(stderr, "[Niri Workspace Windows] WindowClosed Here\n");
        int64_t id = ev->child->valueint;
        khint_t k = Windows_get(windows, id);
        if (k != kh_end(windows)) {
            free(kh_val(windows, k).app_id);
            kh_val(windows, k).app_id = NULL;
            Windows_del(windows, k);
        }
        if (current_focused_window == id) {
            current_focused_window = -1;
        }
        has_event = 1;
    } else {
        fprintf(stderr, "[Niri Workspace Windows] Unhandled: %s\n", ev->string);
    }

    mtx_unlock(&data_lock);
    return has_event;
}

struct ipc_arg {
    int socketfd;
    wbcffi_module* m;
    void (*queue_update)(wbcffi_module*);
};

int ipc(void* arg) {
    struct ipc_arg* ipc_arg = arg;
    int socketfd = ipc_arg->socketfd;
    wbcffi_module* m = ipc_arg->m;
    void (*queue_update)(wbcffi_module*) = ipc_arg->queue_update;

    GInputStream* unix_istream = g_unix_input_stream_new(socketfd, TRUE);
    GOutputStream* unix_ostream = g_unix_output_stream_new(socketfd, FALSE);
    GDataInputStream* istream = g_data_input_stream_new(unix_istream);
    GDataOutputStream* ostream = g_data_output_stream_new(unix_ostream);

    GError* e;
    if (!g_data_output_stream_put_string(
            ostream, "\"EventStream\"\n", NULL, &e)) {
        fprintf(
            stderr,
            "[Niri Workspace Windows] Cannot write to socket: %s\n",
            e->message);
        return 1;
    }

    gsize len = 0;
    char* r = g_data_input_stream_read_line(istream, &len, NULL, &e);
    if (!r) {
        fprintf(
            stderr,
            "[Niri Workspace Windows] Cannot read from socket: %s\n",
            e->message);
        return 1;
    }
    if (strcmp(r, "{\"Ok\":\"Handled\"}")) {
        fprintf(
            stderr, "[Niri Workspace Windows] Failed to start event stream\n");
        return 1;
    }

    windows = Windows_init();
    while ((r = g_data_input_stream_read_line(istream, &len, NULL, &e))) {
        if (parse_ipc(r, len, m, queue_update)) {
            queue_update(m);
        }
        g_free(r);
    }

    free(ipc_arg);
    return 0;
}

void spawn_ipc(
    int socketfd,
    wbcffi_module* m,
    void (*queue_update)(wbcffi_module*)) {
    mtx_init(&data_lock, mtx_plain);
    thrd_t thread;
    struct ipc_arg* ipc_arg = malloc(sizeof(*ipc_arg));
    *ipc_arg = (struct ipc_arg){
        .socketfd = socketfd,
        .m = m,
        .queue_update = queue_update,
    };
    thrd_create(&thread, ipc, ipc_arg);
    thrd_detach(thread);
}

void* wbcffi_init(
    const wbcffi_init_info* init_info,
    const wbcffi_config_entry* config_entries,
    size_t config_entries_len) {
    for (size_t i = 0; i < config_entries_len; i++) {
        const char* key = config_entries[i].key;
        const char* value = config_entries[i].value;
        if (!strcmp(key, "icon_size")) {
            char* endptr = NULL;
            errno = 0;
            gint icon_size = strtol(value, &endptr, 10);
            if (errno || *endptr != '\n') {
                fputs(
                    "[Niri Workspace Windows] \"icon_size\" must be a number\n",
                    stderr);
                return NULL;
            }
            ICON_SIZE = icon_size;
        }
    }

    WBObject* inst = malloc(sizeof(WBObject));
    inst->waybar_module = init_info->obj;

    int socketfd = connect_to_niri();
    if (socketfd == -1) {
        return NULL;
    }
    spawn_ipc(socketfd, init_info->obj, init_info->queue_update);

    GtkContainer* root = init_info->get_root_widget(init_info->obj);

    inst->container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_widget_set_name(GTK_WIDGET(inst->container), "workspace-windows");
    gtk_container_add(GTK_CONTAINER(root), GTK_WIDGET(inst->container));

    return inst;
}

void wbcffi_deinit(void* instance) {
    // TBH I don't care the memory created in this process
    free(instance);
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
    // TODO: Add cache?
    GArray* search_prefixes = get_search_prefixes();
    char* app_folders[] = {"", "applications/"};
    char* suffixes[] = {"", ".desktop"};
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
                        app_id = s;
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
    GtkImage* img =
        GTK_IMAGE(gtk_image_new_from_icon_name(app_id, GTK_ICON_SIZE_INVALID));
    gtk_image_set_pixel_size(img, ICON_SIZE);
    GtkButton* btn = GTK_BUTTON(gtk_button_new());
    gtk_button_set_image(btn, GTK_WIDGET(img));

    return GTK_WIDGET(btn);
}

// Just remove all elements and repopulate container contents
void wbcffi_update(void* inst) {
    if (current_focused_workspace == -1 || current_focused_window == -1) {
        return;
    }
    WBObject* instance = inst;
    khint_t k;
    size_t current_workspace_n_windows = 0;
    kh_foreach(windows, k) {
        current_workspace_n_windows +=
            kh_val(windows, k).workspace_id == current_focused_workspace;
    }
    Window ws[current_workspace_n_windows];
    size_t i = 0;
    kh_foreach(windows, k) {
        if (kh_val(windows, k).workspace_id == current_focused_workspace) {
            ws[i] = kh_val(windows, k);
            i += 1;
        }
    }
    qsort(ws, current_workspace_n_windows, sizeof(*ws), compare_window);

    GList* children =
        gtk_container_get_children(GTK_CONTAINER(instance->container));
    while (children) {
        gtk_container_remove(
            GTK_CONTAINER(instance->container), children->data);
        children = children->next;
    }

    for (size_t i = 0; i < current_workspace_n_windows; i++) {
        GtkWidget* widget = widget_from_app_id(ws[i].app_id);
        GtkStyleContext* context = gtk_widget_get_style_context(widget);
        if (ws[i].id == current_focused_window) {
            gtk_style_context_add_class(context, "focused");
        } else {
            gtk_style_context_remove_class(context, "focused");
        }
        gtk_box_pack_start(instance->container, widget, FALSE, FALSE, 0);
        gtk_box_reorder_child(instance->container, widget, i);
        gtk_widget_show_all(GTK_WIDGET(instance->container));
    }
}

void wbcffi_refresh(void* instance, int signal) {
    // What does this do?
}

void wbcffi_doaction(void* instance, const char* name) {
    // No actions supported
}
