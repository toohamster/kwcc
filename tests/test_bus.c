/* test_bus.c — Standalone test for kwcc_bus (pure C, no mquickjs)
 *
 * Tests:
 *   1.  init + empty publish
 *   2.  subscribe + publish once
 *   3.  unsubscribe
 *   4.  unsubscribe does not affect others
 *   5.  exact match
 *   6.  prefix match
 *   7.  wildcard match
 *   8.  no match no crash
 *   9.  multiple topics independent
 *  10.  sub_id unique
 *  11.  unsubscribe nonexistent
 *  12.  data passthrough
 *  13.  same topic multiple subscribers all triggered
 *  14.  sanitize: "net*work" → "network"
 *  15.  sanitize: "network/ *" → "network/ *"  (wildcard preserved)
 *  16.  topic_check rejects empty string
 *  17.  topic_check rejects all-slash
 *  18.  subscribe invalid topic returns 0
 *  19.  group full returns 0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "src/kwcc_bus.h"
#include "src/kwcc_base.h"
#include "src/llog.h"

static int g_call_count = 0;
static void *g_last_user_data = NULL;

static void cb_count(const char *topic, const void *data, size_t len, void *user_data) {
    (void)topic; (void)data; (void)len;
    g_call_count++;
    g_last_user_data = user_data;
}

static void cb_ignore(const char *topic, const void *data, size_t len, void *user_data) {
    (void)topic; (void)data; (void)len; (void)user_data;
    g_call_count++;
}

#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", #name); } \
    else { printf("  FAIL: %s\n", #name); } \
} while (0)

int main(void) {
    int passed = 0, failed = 0;

    /* 1. init + empty publish */
    kwcc_bus_init();
    kwcc_bus_publish("any/topic", NULL, 0);
    TEST(init_empty_publish, 1);
    passed++;

    /* 2. subscribe + publish once */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_sub_id_t id = kwcc_bus_subscribe("test/topic", cb_count, (void*)0x123);
    kwcc_bus_publish("test/topic", NULL, 0);
    TEST(subscribe_publish_once, id > 0 && g_call_count == 1);
    if (id > 0 && g_call_count == 1) passed++; else failed++;

    /* 3. unsubscribe */
    g_call_count = 0;
    kwcc_bus_unsubscribe(id);
    kwcc_bus_publish("test/topic", NULL, 0);
    TEST(unsubscribe, g_call_count == 0);
    if (g_call_count == 0) passed++; else failed++;

    /* 4. unsubscribe does not affect others */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_sub_id_t id1 = kwcc_bus_subscribe("shared", cb_count, NULL);
    kwcc_bus_sub_id_t id2 = kwcc_bus_subscribe("shared", cb_ignore, NULL);
    kwcc_bus_unsubscribe(id1);
    kwcc_bus_publish("shared", NULL, 0);
    TEST(unsubscribe_not_affect_others, g_call_count == 1);
    if (g_call_count == 1) passed++; else failed++;

    /* 5. exact match */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_subscribe("exact/path", cb_count, NULL);
    kwcc_bus_publish("exact/path", NULL, 0);
    kwcc_bus_publish("exact/path/extra", NULL, 0);
    TEST(exact_match, g_call_count == 1);
    if (g_call_count == 1) passed++; else failed++;

    /* 6. prefix match */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_subscribe("http/", cb_count, NULL);
    kwcc_bus_publish("http/end/req_1", NULL, 0);
    kwcc_bus_publish("http/progress/req_1", NULL, 0);
    kwcc_bus_publish("other/topic", NULL, 0);
    TEST(prefix_match, g_call_count == 2);
    if (g_call_count == 2) passed++; else failed++;

    /* 7. wildcard match */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_subscribe("/*", cb_count, NULL);
    kwcc_bus_publish("anything/goes", NULL, 0);
    kwcc_bus_publish("x", NULL, 0);
    TEST(wildcard_match, g_call_count == 2);
    if (g_call_count == 2) passed++; else failed++;

    /* 8. no match no crash */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_subscribe("foo/bar", cb_count, NULL);
    kwcc_bus_publish("different/topic", NULL, 0);
    TEST(no_match_no_crash, g_call_count == 0);
    if (g_call_count == 0) passed++; else failed++;

    /* 9. multiple topics independent */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_subscribe("topic/a", cb_count, NULL);
    kwcc_bus_subscribe("topic/b", cb_ignore, NULL);
    kwcc_bus_publish("topic/a", NULL, 0);
    int c1 = g_call_count;
    kwcc_bus_publish("topic/b", NULL, 0);
    int c2 = g_call_count;
    TEST(multiple_topics_independent, c1 == 1 && c2 == 2);
    if (c1 == 1 && c2 == 2) passed++; else failed++;

    /* 10. sub_id unique */
    kwcc_bus_init();
    kwcc_bus_sub_id_t s1 = kwcc_bus_subscribe("t1", cb_count, NULL);
    kwcc_bus_sub_id_t s2 = kwcc_bus_subscribe("t2", cb_count, NULL);
    TEST(sub_id_unique, s1 != s2);
    if (s1 != s2) passed++; else failed++;

    /* 11. unsubscribe nonexistent */
    kwcc_bus_unsubscribe(999999);
    TEST(unsubscribe_nonexistent, 1);
    passed++;

    /* 12. data passthrough */
    kwcc_bus_init();
    g_call_count = 0;
    int test_data = 42;
    kwcc_bus_subscribe("data/test", cb_count, &test_data);
    kwcc_bus_publish("data/test", &test_data, sizeof(test_data));
    TEST(data_passthrough, g_call_count == 1 && g_last_user_data == &test_data);
    if (g_call_count == 1 && g_last_user_data == &test_data) passed++; else failed++;

    /* 13. same topic multiple subscribers all triggered */
    kwcc_bus_init();
    g_call_count = 0;
    kwcc_bus_subscribe("multi", cb_count, NULL);
    kwcc_bus_subscribe("multi", cb_ignore, NULL);
    kwcc_bus_publish("multi", NULL, 0);
    TEST(multi_subscribers_all_triggered, g_call_count == 2);
    if (g_call_count == 2) passed++; else failed++;

    /* 14. sanitize: "net*work" → "network" */
    {
        char out[256];
        kwcc_base_topic_sanitize(out, sizeof(out), "net*work");
        TEST(sanitize_strip_star, strcmp(out, "network") == 0);
        if (strcmp(out, "network") == 0) passed++; else failed++;
    }

    /* 15. sanitize: "network/ *" wildcard preserved */
    {
        char out[256];
        kwcc_base_topic_sanitize(out, sizeof(out), "network/*");
        TEST(sanitize_keep_wildcard, strcmp(out, "network/*") == 0);
        if (strcmp(out, "network/*") == 0) passed++; else failed++;
    }

    /* 16. topic_check rejects empty string */
    {
        int ok = kwcc_base_topic_check("");
        TEST(topic_check_empty, ok == 0);
        if (ok == 0) passed++; else failed++;
    }

    /* 17. topic_check rejects all-slash */
    {
        int ok = kwcc_base_topic_check("///");
        TEST(topic_check_all_slash, ok == 0);
        if (ok == 0) passed++; else failed++;
    }

    /* 18. subscribe invalid topic returns 0 */
    {
        kwcc_bus_init();
        kwcc_bus_sub_id_t sid = kwcc_bus_subscribe("", cb_count, NULL);
        TEST(subscribe_invalid_topic, sid == 0);
        if (sid == 0) passed++; else failed++;
    }

    /* 19. group full returns 0 */
    {
        kwcc_bus_init();
        kwcc_bus_sub_id_t last = 1;
        for (int i = 0; i < 17; i++) {
            last = kwcc_bus_subscribe("full/group", cb_count, NULL);
        }
        TEST(group_full_returns_0, last == 0);
        if (last == 0) passed++; else failed++;
    }

    printf("\n%d/%d tests passed.\n", passed, passed + failed);
    return failed > 0 ? 1 : 0;
}
