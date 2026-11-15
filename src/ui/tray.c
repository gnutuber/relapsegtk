#include "tray.h"

#include <string.h>

enum { MENU_OPEN = 1, MENU_SEPARATOR = 2, MENU_QUIT = 3 };

struct Tray {
    GtkWindow *window;
    TrayActivateFunc activate;
    TrayActivateFunc quit;
    gpointer user_data;
    GDBusConnection *bus;
    GBytes *pixmap;
    int pixmap_width;
    int pixmap_height;
    guint item_id;
    guint menu_id;
    guint watcher_id;
    guint revision;
};

static const char *ITEM_XML =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='AttentionMovieName' type='s' access='read'/>"
    "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "    <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "    <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "    <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "    <signal name='NewTitle'/>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewAttentionIcon'/>"
    "    <signal name='NewOverlayIcon'/>"
    "    <signal name='NewToolTip'/>"
    "    <signal name='NewStatus'><arg type='s'/></signal>"
    "  </interface>"
    "</node>";

static const char *MENU_XML =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <method name='GetLayout'>"
    "      <arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/>"
    "      <arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg type='ai' direction='in'/><arg type='as' direction='in'/>"
    "      <arg type='a(ia{sv})' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/>"
    "      <arg type='u' direction='in'/>"
    "    </method>"
    "    <method name='EventGroup'><arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/></method>"
    "    <method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "    <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='TextDirection' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='IconThemePath' type='as' access='read'/>"
    "  </interface>"
    "</node>";

static const char *menu_label(int id)
{
    if (id == MENU_OPEN) return "Open Relapse";
    if (id == MENU_QUIT) return "Quit";
    return NULL;
}

static const char *menu_type(int id)
{
    return id == MENU_SEPARATOR ? "separator" : NULL;
}

static gboolean item_properties(int id, GVariantBuilder *builder)
{
    if (id == 0) return TRUE;
    if (id != MENU_OPEN && id != MENU_SEPARATOR && id != MENU_QUIT) return FALSE;
    const char *label = menu_label(id);
    const char *type = menu_type(id);
    if (label) g_variant_builder_add(builder, "{sv}", "label", g_variant_new_string(label));
    if (type) g_variant_builder_add(builder, "{sv}", "type", g_variant_new_string(type));
    g_variant_builder_add(builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
    return TRUE;
}

static GVariant *item_layout(int id)
{
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    item_properties(id, &properties);
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (id == 0) {
        for (int child = MENU_OPEN; child <= MENU_QUIT; ++child)
            g_variant_builder_add(&children, "v", item_layout(child));
    }
    return g_variant_new("(i@a{sv}@av)", id, g_variant_builder_end(&properties),
        g_variant_builder_end(&children));
}

static GVariant *icon_pixmap(Tray *tray)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    gsize size = 0;
    const guint8 *data = tray->pixmap ? g_bytes_get_data(tray->pixmap, &size) : NULL;
    if (data && size)
        g_variant_builder_add(&builder, "(ii@ay)", tray->pixmap_width, tray->pixmap_height,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, size, 1));
    return g_variant_builder_end(&builder);
}

static GVariant *item_property(Tray *tray, const char *name)
{
    if (g_str_equal(name, "Category")) return g_variant_new_string("ApplicationStatus");
    if (g_str_equal(name, "Id")) return g_variant_new_string("relapsegtk");
    if (g_str_equal(name, "Title")) return g_variant_new_string("Relapse");
    if (g_str_equal(name, "Status")) return g_variant_new_string("Active");
    if (g_str_equal(name, "WindowId")) return g_variant_new_int32(0);
    if (g_str_equal(name, "IconName")) return g_variant_new_string("");
    if (g_str_equal(name, "IconPixmap")) return icon_pixmap(tray);
    if (g_str_equal(name, "OverlayIconName")) return g_variant_new_string("");
    if (g_str_equal(name, "AttentionIconName")) return g_variant_new_string("");
    if (g_str_equal(name, "AttentionMovieName")) return g_variant_new_string("");
    if (g_str_equal(name, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (g_str_equal(name, "Menu")) return g_variant_new_object_path("/MenuBar");
    if (g_str_equal(name, "ToolTip"))
        return g_variant_new("(s@a(iiay)ss)", "", icon_pixmap(tray), "Relapse", "");
    return NULL;
}

static void tray_activate(Tray *tray)
{
    if (tray->activate) tray->activate(tray->user_data);
}

static void tray_quit(Tray *tray)
{
    if (tray->quit) tray->quit(tray->user_data);
}

static GVariant *item_method(Tray *tray, const char *method)
{
    if (g_str_equal(method, "Activate") || g_str_equal(method, "SecondaryActivate"))
        tray_activate(tray);
    return NULL;
}

static GVariant *menu_method(Tray *tray, const char *method, GVariant *parameters)
{
    if (g_str_equal(method, "GetLayout")) {
        g_autoptr(GVariant) layout = g_variant_ref_sink(item_layout(0));
        return g_variant_new("(u@(ia{sv}av))", tray->revision, layout);
    }
    if (g_str_equal(method, "GetGroupProperties")) {
        g_autoptr(GVariant) requested = g_variant_get_child_value(parameters, 0);
        gsize count = g_variant_n_children(requested);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
        if (!count) {
            for (int id = 0; id <= MENU_QUIT; ++id) {
                GVariantBuilder properties;
                g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
                item_properties(id, &properties);
                g_variant_builder_add(&builder, "(i@a{sv})", id, g_variant_builder_end(&properties));
            }
        } else {
            for (gsize i = 0; i < count; ++i) {
                gint32 id = 0;
                g_variant_get_child(requested, i, "i", &id);
                GVariantBuilder properties;
                g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
                item_properties(id, &properties);
                g_variant_builder_add(&builder, "(i@a{sv})", id, g_variant_builder_end(&properties));
            }
        }
        return g_variant_new("(a(ia{sv}))", &builder);
    }
    if (g_str_equal(method, "GetProperty")) {
        gint32 id = 0;
        const char *property = NULL;
        g_variant_get(parameters, "(i&s)", &id, &property);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        item_properties(id, &builder);
        g_autoptr(GVariant) values = g_variant_builder_end(&builder);
        g_autoptr(GVariant) value = g_variant_lookup_value(values, property, NULL);
        return g_variant_new("(v)", value ? value : g_variant_new_string(""));
    }
    if (g_str_equal(method, "Event")) {
        gint32 id = 0;
        const char *event = NULL;
        g_variant_get(parameters, "(i&svu)", &id, &event, NULL, NULL);
        if (g_str_equal(event, "clicked")) {
            if (id == MENU_OPEN) tray_activate(tray);
            else if (id == MENU_QUIT) tray_quit(tray);
        }
        return NULL;
    }
    if (g_str_equal(method, "AboutToShow")) return g_variant_new("(b)", FALSE);
    if (g_str_equal(method, "EventGroup"))
        return g_variant_new("(ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0));
    return NULL;
}

static void item_call(GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *method, GVariant *parameters,
                      GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)sender; (void)path; (void)parameters;
    Tray *tray = user_data;
    if (!g_str_equal(interface, "org.kde.StatusNotifierItem")) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown interface %s", interface);
        return;
    }
    g_autoptr(GVariant) result = item_method(tray, method);
    if (result) g_dbus_method_invocation_return_value(invocation, result);
    else g_dbus_method_invocation_return_value(invocation, NULL);
    (void)connection;
}

static void menu_call(GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *method, GVariant *parameters,
                      GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)sender; (void)path; (void)connection;
    Tray *tray = user_data;
    if (!g_str_equal(interface, "com.canonical.dbusmenu")) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown interface %s", interface);
        return;
    }
    GVariant *result = menu_method(tray, method, parameters);
    if (result) g_dbus_method_invocation_return_value(invocation, result);
    else g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *item_get(GDBusConnection *connection, const char *sender, const char *path,
                          const char *interface, const char *property, GError **error,
                          gpointer user_data)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    Tray *tray = user_data;
    GVariant *value = item_property(tray, property);
    if (!value) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
        "Unknown property %s", property);
    return value;
}

static GVariant *menu_get(GDBusConnection *connection, const char *sender, const char *path,
                          const char *interface, const char *property, GError **error,
                          gpointer user_data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)user_data; (void)error;
    if (g_str_equal(property, "Version")) return g_variant_new_uint32(3);
    if (g_str_equal(property, "TextDirection")) return g_variant_new_string("ltr");
    if (g_str_equal(property, "Status")) return g_variant_new_string("normal");
    if (g_str_equal(property, "IconThemePath"))
        return g_variant_new_strv(NULL, 0);
    return NULL;
}

static GBytes *load_pixmap(int *width, int *height)
{
    g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_resource_at_scale(
        "/org/relapse/RelapseGtk/icons/relapse.svg", 22, 22, TRUE, NULL);
    if (!pixbuf) pixbuf = gdk_pixbuf_new_from_resource_at_scale(
        "/org/relapse/RelapseGtk/icons/autoexec.png", 22, 22, TRUE, NULL);
    if (!pixbuf) return NULL;
    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    gboolean alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
    g_autoptr(GByteArray) data = g_byte_array_sized_new(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const guchar *source = pixels + y * stride + x * (alpha ? 4 : 3);
            guint8 pixel[4] = {alpha ? source[3] : 255, source[0], source[1], source[2]};
            if (pixel[0] != 0 && pixel[0] != 255) {
                for (int channel = 1; channel < 4; ++channel)
                    pixel[channel] = (guint8)((pixel[channel] * pixel[0] + 127) / 255);
            }
            g_byte_array_append(data, pixel, 4);
        }
    }
    *width = w;
    *height = h;
    return g_byte_array_free_to_bytes(g_steal_pointer(&data));
}

static void watcher_appeared(GDBusConnection *connection, const char *name, const char *owner,
                             gpointer user_data)
{
    (void)name; (void)owner;
    Tray *tray = user_data;
    g_dbus_connection_call(connection, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", g_dbus_connection_get_unique_name(connection)), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    tray->revision++;
}

static void watcher_vanished(GDBusConnection *connection, const char *name, gpointer user_data)
{
    (void)connection; (void)name; (void)user_data;
}

static gboolean watcher_present(GDBusConnection *connection)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.kde.StatusNotifierWatcher"), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (!reply) return FALSE;
    gboolean owner = FALSE;
    g_variant_get(reply, "(b)", &owner);
    return owner;
}

Tray *tray_new(GtkWindow *window, TrayActivateFunc activate, TrayActivateFunc quit, gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        if (error) g_printerr("No session bus for the tray icon: %s\n", error->message);
        return NULL;
    }
    if (!watcher_present(bus)) {
        g_object_unref(bus);
        return NULL;
    }
    GDBusNodeInfo *item_info = g_dbus_node_info_new_for_xml(ITEM_XML, NULL);
    GDBusNodeInfo *menu_info = g_dbus_node_info_new_for_xml(MENU_XML, NULL);
    if (!item_info || !menu_info) {
        if (item_info) g_dbus_node_info_unref(item_info);
        if (menu_info) g_dbus_node_info_unref(menu_info);
        g_object_unref(bus);
        return NULL;
    }
    Tray *tray = g_new0(Tray, 1);
    tray->window = window;
    tray->activate = activate;
    tray->quit = quit;
    tray->user_data = user_data;
    tray->bus = bus;
    tray->revision = 1;
    tray->pixmap = load_pixmap(&tray->pixmap_width, &tray->pixmap_height);
    tray->item_id = g_dbus_connection_register_object(bus, "/StatusNotifierItem",
        item_info->interfaces[0],
        &(GDBusInterfaceVTable){.method_call = item_call, .get_property = item_get},
        tray, NULL, &error);
    tray->menu_id = g_dbus_connection_register_object(bus, "/MenuBar", menu_info->interfaces[0],
        &(GDBusInterfaceVTable){.method_call = menu_call, .get_property = menu_get},
        tray, NULL, &error);
    if (!tray->item_id || !tray->menu_id) {
        if (error) g_printerr("Could not export the tray icon: %s\n", error->message);
        g_dbus_node_info_unref(item_info);
        g_dbus_node_info_unref(menu_info);
        tray_free(tray);
        return NULL;
    }
    g_dbus_node_info_unref(item_info);
    g_dbus_node_info_unref(menu_info);
    tray->watcher_id = g_bus_watch_name_on_connection(bus, "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_appeared, watcher_vanished, tray, NULL);
    return tray;
}

void tray_free(Tray *tray)
{
    if (!tray) return;
    if (tray->watcher_id) g_bus_unwatch_name(tray->watcher_id);
    if (tray->item_id) g_dbus_connection_unregister_object(tray->bus, tray->item_id);
    if (tray->menu_id) g_dbus_connection_unregister_object(tray->bus, tray->menu_id);
    g_clear_pointer(&tray->pixmap, g_bytes_unref);
    if (tray->bus) g_object_unref(tray->bus);
    g_free(tray);
}
