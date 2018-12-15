#define wake_up wake_up_void
#include "../src/dbus.c"
#include "greatest.h"

#include <assert.h>
#include <gio/gio.h>

#include "queues.h"

void wake_up_void(void) {  }

struct signal_closed {
        guint32 id;
        guint32 reason;
        guint subscription_id;
        GDBusConnection *conn;
};

void dbus_signal_cb_closed(GDBusConnection *connection,
                 const gchar *sender_name,
                 const gchar *object_path,
                 const gchar *interface_name,
                 const gchar *signal_name,
                 GVariant *parameters,
                 gpointer user_data)
{
        g_return_if_fail(user_data);

        guint32 id;
        guint32 reason;

        struct signal_closed *sig = (struct signal_closed*) user_data;
        g_variant_get(parameters, "(uu)", &id, &reason);

        if (id == sig->id) {
                sig->id = id;
                sig->reason = reason;
        }
}

void dbus_signal_subscribe_closed(struct signal_closed *closed)
{
        assert(closed);

        closed->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        closed->subscription_id =
                g_dbus_connection_signal_subscribe(
                        closed->conn,
                        FDN_NAME,
                        FDN_IFAC,
                        "NotificationClosed",
                        FDN_PATH,
                        NULL,
                        G_DBUS_SIGNAL_FLAGS_NONE,
                        dbus_signal_cb_closed,
                        closed,
                        NULL);
}

void dbus_signal_unsubscribe_closed(struct signal_closed *closed)
{
        assert(closed);

        g_dbus_connection_signal_unsubscribe(closed->conn, closed->subscription_id);
        g_object_unref(closed->conn);

        closed->conn = NULL;
        closed->subscription_id = -1;
}

GVariant *dbus_invoke(const char *method, GVariant *params)
{
        GDBusConnection *connection_client;
        GVariant *retdata;
        GError *error = NULL;

        connection_client = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        retdata = g_dbus_connection_call_sync(
                                connection_client,
                                FDN_NAME,
                                FDN_PATH,
                                FDN_IFAC,
                                method,
                                params,
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                &error);
        if (error) {
                printf("Error while calling GTestDBus instance: %s\n", error->message);
                g_error_free(error);
        }

        g_object_unref(connection_client);

        return retdata;
}

struct dbus_notification {
        const char* app_name;
        guint replaces_id;
        const char* app_icon;
        const char* summary;
        const char* body;
        GHashTable *actions;
        GHashTable *hints;
        int expire_timeout;
};

void g_free_variant_value(gpointer tofree)
{
        g_variant_unref((GVariant*) tofree);
}

struct dbus_notification *dbus_notification_new(void)
{
        struct dbus_notification *n = g_malloc0(sizeof(struct dbus_notification));
        n->expire_timeout = -1;
        n->replaces_id = 0;
        n->actions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        n->hints = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free_variant_value);
        return n;
}

void dbus_notification_free(struct dbus_notification *n)
{
        g_hash_table_unref(n->hints);
        g_hash_table_unref(n->actions);
        g_free(n);
}

bool dbus_notification_fire(struct dbus_notification *n, uint *id)
{
        assert(n);
        assert(id);
        GVariantBuilder b;
        GVariantType *t;

        gpointer p_key;
        gpointer p_value;
        GHashTableIter iter;

        t = g_variant_type_new("(susssasa{sv}i)");
        g_variant_builder_init(&b, t);
        g_variant_type_free(t);

        g_variant_builder_add(&b, "s", n->app_name);
        g_variant_builder_add(&b, "u", n->replaces_id);
        g_variant_builder_add(&b, "s", n->app_icon);
        g_variant_builder_add(&b, "s", n->summary);
        g_variant_builder_add(&b, "s", n->body);

        // Add the actions
        t = g_variant_type_new("as");
        g_variant_builder_open(&b, t);
        g_hash_table_iter_init(&iter, n->actions);
        while (g_hash_table_iter_next(&iter, &p_key, &p_value)) {
                g_variant_builder_add(&b, "s", (char*)p_key);
                g_variant_builder_add(&b, "s", (char*)p_value);
        }
        g_variant_builder_close(&b);
        g_variant_type_free(t);

        // Add the hints
        t = g_variant_type_new("a{sv}");
        g_variant_builder_open(&b, t);
        g_hash_table_iter_init(&iter, n->hints);
        while (g_hash_table_iter_next(&iter, &p_key, &p_value)) {
                g_variant_builder_add(&b, "{sv}", (char*)p_key, (GVariant*)p_value);
        }
        g_variant_builder_close(&b);
        g_variant_type_free(t);

        g_variant_builder_add(&b, "i", n->expire_timeout);

        GVariant *reply = dbus_invoke("Notify", g_variant_builder_end(&b));
        if (reply) {
                g_variant_get(reply, "(u)", id);
                g_variant_unref(reply);
                return true;
        } else {
                return false;
        }
}

/////// TESTS
gint owner_id;

TEST test_dbus_init(void)
{
        owner_id = dbus_init();
        uint waiting = 0;
        while (!dbus_conn && waiting < 2000) {
                usleep(500);
                waiting++;
        }
        ASSERTm("After 1s, there is still no dbus connection available.",
                dbus_conn);
        PASS();
}

TEST test_dbus_teardown(void)
{
        dbus_teardown(owner_id);
        PASS();
}

TEST test_invalid_notification(void)
{
        GVariant *faulty = g_variant_new_boolean(true);

        ASSERT(NULL == dbus_message_to_notification(":123", faulty));
        ASSERT(NULL == dbus_invoke("Notify", faulty));

        g_variant_unref(faulty);
        PASS();
}

TEST test_empty_notification(void)
{
        struct dbus_notification *n = dbus_notification_new();
        gsize len = queues_length_waiting();

        guint id;
        ASSERT(dbus_notification_fire(n, &id));
        ASSERT(id != 0);

        ASSERT_EQ(queues_length_waiting(), len+1);
        dbus_notification_free(n);
        PASS();
}

TEST test_basic_notification(void)
{
        struct dbus_notification *n = dbus_notification_new();
        gsize len = queues_length_waiting();
        n->app_name = "dunstteststack";
        n->app_icon = "NONE";
        n->summary = "Headline";
        n->body = "Text";
        g_hash_table_insert(n->actions, g_strdup("actionid"), g_strdup("Print this text"));
        g_hash_table_insert(n->hints,
                            g_strdup("x-dunst-stack-tag"),
                            g_variant_ref_sink(g_variant_new_string("volume")));

        n->replaces_id = 10;

        guint id;
        ASSERT(dbus_notification_fire(n, &id));
        ASSERT(id != 0);

        ASSERT_EQ(queues_length_waiting(), len+1);
        dbus_notification_free(n);
        PASS();
}

TEST test_dbus_notify_colors(void)
{
        const char *color_frame = "I allow all string values for frame!";
        const char *color_bg = "I allow all string values for background!";
        const char *color_fg = "I allow all string values for foreground!";
        struct notification *n;
        struct dbus_notification *n_dbus;

        gsize len = queues_length_waiting();

        n_dbus = dbus_notification_new();
        n_dbus->app_name = "dunstteststack";
        n_dbus->app_icon = "NONE";
        n_dbus->summary = "test_dbus_notify_colors";
        n_dbus->body = "Summary of it";
        g_hash_table_insert(n_dbus->hints,
                            g_strdup("frcolor"),
                            g_variant_ref_sink(g_variant_new_string(color_frame)));
        g_hash_table_insert(n_dbus->hints,
                            g_strdup("bgcolor"),
                            g_variant_ref_sink(g_variant_new_string(color_bg)));
        g_hash_table_insert(n_dbus->hints,
                            g_strdup("fgcolor"),
                            g_variant_ref_sink(g_variant_new_string(color_fg)));

        guint id;
        ASSERT(dbus_notification_fire(n_dbus, &id));
        ASSERT(id != 0);

        ASSERT_EQ(queues_length_waiting(), len+1);

        n = queues_debug_find_notification_by_id(id);

        ASSERT_STR_EQ(n->colors.frame, color_frame);
        ASSERT_STR_EQ(n->colors.fg, color_fg);
        ASSERT_STR_EQ(n->colors.bg, color_bg);

        dbus_notification_free(n_dbus);

        PASS();
}

TEST test_hint_transient(void)
{
        static char msg[50];
        struct notification *n;
        struct dbus_notification *n_dbus;

        gsize len = queues_length_waiting();

        n_dbus = dbus_notification_new();
        n_dbus->app_name = "dunstteststack";
        n_dbus->app_icon = "NONE";
        n_dbus->summary = "test_hint_transient";
        n_dbus->body = "Summary of it";

        bool values[] = { true, true, true, false, false, false, false };
        GVariant *variants[] = {
                g_variant_new_boolean(true),
                g_variant_new_int32(1),
                g_variant_new_uint32(1),
                g_variant_new_boolean(false),
                g_variant_new_int32(0),
                g_variant_new_uint32(0),
                g_variant_new_int32(-1),
        };
        for (size_t i = 0; i < G_N_ELEMENTS(variants); i++) {
                g_hash_table_insert(n_dbus->hints,
                                    g_strdup("transient"),
                                    g_variant_ref_sink(variants[i]));

                guint id;
                ASSERT(dbus_notification_fire(n_dbus, &id));
                ASSERT(id != 0);

                ASSERT_EQ(queues_length_waiting(), len+1);

                n = queues_debug_find_notification_by_id(id);

                snprintf(msg, sizeof(msg), "In round %ld", i);
                ASSERT_EQm(msg, values[i], n->transient);
        }

        dbus_notification_free(n_dbus);

        PASS();
}

TEST test_hint_progress(void)
{
        static char msg[50];
        struct notification *n;
        struct dbus_notification *n_dbus;

        gsize len = queues_length_waiting();

        n_dbus = dbus_notification_new();
        n_dbus->app_name = "dunstteststack";
        n_dbus->app_icon = "NONE";
        n_dbus->summary = "test_hint_progress";
        n_dbus->body = "Summary of it";

        int values[] = { 99, 12, 123, 123, -1, -1 };
        GVariant *variants[] = {
                g_variant_new_int32(99),
                g_variant_new_uint32(12),
                g_variant_new_int32(123), // allow higher than 100
                g_variant_new_uint32(123),
                g_variant_new_int32(-192),
                g_variant_new_uint32(-192),
        };
        for (size_t i = 0; i < G_N_ELEMENTS(variants); i++) {
                g_hash_table_insert(n_dbus->hints,
                                    g_strdup("value"),
                                    g_variant_ref_sink(variants[i]));

                guint id;
                ASSERT(dbus_notification_fire(n_dbus, &id));
                ASSERT(id != 0);

                ASSERT_EQ(queues_length_waiting(), len+1);

                n = queues_debug_find_notification_by_id(id);

                snprintf(msg, sizeof(msg), "In round %ld", i);
                ASSERT_EQm(msg, values[i], n->progress);
        }

        dbus_notification_free(n_dbus);

        PASS();
}

TEST test_hint_icons(void)
{
        struct notification *n;
        struct dbus_notification *n_dbus;
        const char *iconname = "NEWICON";

        gsize len = queues_length_waiting();

        n_dbus = dbus_notification_new();
        n_dbus->app_name = "dunstteststack";
        n_dbus->app_icon = "NONE";
        n_dbus->summary = "test_hint_icons";
        n_dbus->body = "Summary of it";

        g_hash_table_insert(n_dbus->hints,
                            g_strdup("image-path"),
                            g_variant_ref_sink(g_variant_new_string(iconname)));

        guint id;
        ASSERT(dbus_notification_fire(n_dbus, &id));
        ASSERT(id != 0);

        ASSERT_EQ(queues_length_waiting(), len+1);

        n = queues_debug_find_notification_by_id(id);

        ASSERT_STR_EQ(iconname, n->icon);

        dbus_notification_free(n_dbus);

        PASS();
}

TEST test_server_caps(enum markup_mode markup)
{
        GVariant *reply;
        GVariant *caps = NULL;
        const char **capsarray;

        settings.markup = markup;

        reply = dbus_invoke("GetCapabilities", NULL);

        caps = g_variant_get_child_value(reply, 0);
        capsarray = g_variant_get_strv(caps, NULL);

        ASSERT(capsarray);
        ASSERT(g_strv_contains(capsarray, "actions"));
        ASSERT(g_strv_contains(capsarray, "body"));
        ASSERT(g_strv_contains(capsarray, "body-hyperlinks"));
        ASSERT(g_strv_contains(capsarray, "x-dunst-stack-tag"));

        if (settings.markup != MARKUP_NO)
                ASSERT(g_strv_contains(capsarray, "body-markup"));
        else
                ASSERT_FALSE(g_strv_contains(capsarray, "body-markup"));

        g_free(capsarray);
        g_variant_unref(caps);
        g_variant_unref(reply);
        PASS();
}

TEST test_close_and_signal(void)
{
        GVariant *data, *ret;
        struct dbus_notification *n;
        struct signal_closed sig = {0, REASON_MIN-1, -1};

        dbus_signal_subscribe_closed(&sig);

        n = dbus_notification_new();
        n->app_name = "dunstteststack";
        n->app_icon = "NONE2";
        n->summary = "Headline for New";
        n->body = "Text";

        dbus_notification_fire(n, &sig.id);

        data = g_variant_new("(u)", sig.id);
        ret = dbus_invoke("CloseNotification", data);

        ASSERT(ret);

        uint waiting = 0;
        while (sig.reason == REASON_MIN-1 && waiting < 2000) {
                usleep(500);
                waiting++;
        }

        ASSERT(sig.reason != REASON_MIN-1);

        dbus_notification_free(n);
        dbus_signal_unsubscribe_closed(&sig);
        g_variant_unref(ret);
        PASS();
}

TEST test_get_fdn_daemon_info(void)
{
        unsigned int pid_is;
        pid_t pid_should;
        char *name, *vendor;
        GDBusConnection *conn;

        pid_should = getpid();
        conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

        ASSERT(dbus_get_fdn_daemon_info(conn, &pid_is, &name, &vendor));

        ASSERT_EQ_FMT(pid_should, pid_is, "%d");
        ASSERT_STR_EQ("dunst", name);
        ASSERT_STR_EQ("knopwob", vendor);

        g_free(name);
        g_free(vendor);

        g_object_unref(conn);
        PASS();
}

TEST assert_methodlists_sorted(void)
{
        for (size_t i = 0; i+1 < G_N_ELEMENTS(methods_fdn); i++) {
                ASSERT(0 > strcmp(
                                methods_fdn[i].method_name,
                                methods_fdn[i+1].method_name));
        }

        PASS();
}


// TESTS END

GMainLoop *loop;
GThread *thread_tests;

gpointer run_threaded_tests(gpointer data)
{
        RUN_TEST(test_dbus_init);

        RUN_TEST(test_get_fdn_daemon_info);

        RUN_TEST(test_empty_notification);
        RUN_TEST(test_basic_notification);
        RUN_TEST(test_invalid_notification);
        RUN_TEST(test_hint_transient);
        RUN_TEST(test_hint_progress);
        RUN_TEST(test_hint_icons);
        RUN_TEST(test_dbus_notify_colors);
        RUN_TESTp(test_server_caps, MARKUP_FULL);
        RUN_TESTp(test_server_caps, MARKUP_STRIP);
        RUN_TESTp(test_server_caps, MARKUP_NO);
        RUN_TEST(test_close_and_signal);

        RUN_TEST(assert_methodlists_sorted);

        RUN_TEST(test_dbus_teardown);
        g_main_loop_quit(loop);
        return NULL;
}

SUITE(suite_dbus)
{
        GTestDBus *dbus_bus;
        g_test_dbus_unset();
        queues_init();

        loop = g_main_loop_new(NULL, false);

        dbus_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
        g_test_dbus_up(dbus_bus);

        thread_tests = g_thread_new("testexecutor", run_threaded_tests, loop);
        g_main_loop_run(loop);

        queues_teardown();
        g_test_dbus_down(dbus_bus);
        g_object_unref(dbus_bus);
        g_thread_unref(thread_tests);
        g_main_loop_unref(loop);
}

/* vim: set tabstop=8 shiftwidth=8 expandtab textwidth=0: */
