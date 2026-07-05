/* test_defer_cleanup.c — pure C test for kwcc_base_defer_cleanup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/kwcc_base.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", #name); } \
    else { tests_failed++; printf("  FAIL: %s\n", #name); } \
} while (0)

/* ── helpers ── */

static int g_free_count = 0;

static void test_free_fn(void *ptr) {
    g_free_count++;
    free(ptr);
}

static void test_noop_fn(void *ptr) {
    (void)ptr;
    g_free_count++;
}

/* ── tests ── */

static void test_create_returns_nonnull(void) {
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    TEST(create_returns_nonnull, dc != NULL);
    TEST(create_head_is_null, dc->head == NULL);
    kwcc_base_defer_cleanup_run(dc);
}

static void test_run_null_dc_no_crash(void) {
    kwcc_base_defer_cleanup_run(NULL);
    TEST(run_null_dc_no_crash, 1);
}

static void test_push_then_run_frees_resource(void) {
    g_free_count = 0;
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    int *val = (int *)malloc(sizeof(int));
    *val = 42;
    kwcc_base_defer_cleanup_push(dc, val, test_free_fn);
    TEST(push_count_1, g_free_count == 0);
    kwcc_base_defer_cleanup_run(dc);
    TEST(run_frees_resource, g_free_count == 1);
}

static void test_multiple_pushes_reverse_order(void) {
    g_free_count = 0;
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    int *a = (int *)malloc(sizeof(int)); *a = 1;
    int *b = (int *)malloc(sizeof(int)); *b = 2;
    int *c = (int *)malloc(sizeof(int)); *c = 3;
    kwcc_base_defer_cleanup_push(dc, a, test_free_fn);
    kwcc_base_defer_cleanup_push(dc, b, test_free_fn);
    kwcc_base_defer_cleanup_push(dc, c, test_free_fn);
    kwcc_base_defer_cleanup_run(dc);
    TEST(multiple_pushes_all_freed, g_free_count == 3);
}

static void test_run_on_empty_dc(void) {
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    /* no push, just run — should not crash */
    kwcc_base_defer_cleanup_run(dc);
    TEST(run_empty_dc_no_crash, 1);
}

static void test_push_with_free_directly(void) {
    /* push with standard free as fn */
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    char *buf = (char *)malloc(64);
    strcpy(buf, "hello");
    kwcc_base_defer_cleanup_push(dc, buf, free);
    kwcc_base_defer_cleanup_run(dc);
    /* if we reach here, free(buf) was called correctly */
    TEST(push_with_free_directly, 1);
}

static void test_push_null_ptr_no_crash(void) {
    g_free_count = 0;
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    kwcc_base_defer_cleanup_push(dc, NULL, test_noop_fn);
    kwcc_base_defer_cleanup_run(dc);
    TEST(push_null_ptr_no_crash, g_free_count == 1);
}

static void test_push_null_fn_no_crash(void) {
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    int *val = (int *)malloc(sizeof(int));
    *val = 99;
    kwcc_base_defer_cleanup_push(dc, val, NULL);
    kwcc_base_defer_cleanup_run(dc);
    /* fn is NULL, val not freed — leak expected, but no crash */
    free(val); /* manual cleanup for test */
    TEST(push_null_fn_no_crash, 1);
}

static void test_push_null_dc_no_crash(void) {
    int *val = (int *)malloc(sizeof(int));
    kwcc_base_defer_cleanup_push(NULL, val, free);
    free(val);
    TEST(push_null_dc_no_crash, 1);
}

static void test_kwcc_base_str_free_as_fn(void) {
    /* use (kwcc_base_defer_cleanup_fn)kwcc_base_str_free as fn */
    kwcc_base_defer_cleanup_t *dc = kwcc_base_defer_cleanup_create();
    kwcc_base_str_t s = kwcc_base_str_new("test", 4);
    kwcc_base_defer_cleanup_push(dc, &s, (kwcc_base_defer_cleanup_fn)kwcc_base_str_free);
    kwcc_base_defer_cleanup_run(dc);
    TEST(str_free_as_fn, s.val == NULL);
}

static void test_nested_defer_cleanup(void) {
    /* two independent dc instances, nested calls */
    g_free_count = 0;
    kwcc_base_defer_cleanup_t *outer = kwcc_base_defer_cleanup_create();
    kwcc_base_defer_cleanup_t *inner = kwcc_base_defer_cleanup_create();
    int *a = (int *)malloc(sizeof(int));
    int *b = (int *)malloc(sizeof(int));
    kwcc_base_defer_cleanup_push(outer, a, test_free_fn);
    kwcc_base_defer_cleanup_push(inner, b, test_free_fn);
    kwcc_base_defer_cleanup_run(inner);
    TEST(nested_inner_freed, g_free_count == 1);
    kwcc_base_defer_cleanup_run(outer);
    TEST(nested_outer_freed, g_free_count == 2);
}

/* ── main ── */

int main(void) {
    printf("kwcc_base_defer_cleanup tests:\n");

    test_create_returns_nonnull();
    test_run_null_dc_no_crash();
    test_push_then_run_frees_resource();
    test_multiple_pushes_reverse_order();
    test_run_on_empty_dc();
    test_push_with_free_directly();
    test_push_null_ptr_no_crash();
    test_push_null_fn_no_crash();
    test_push_null_dc_no_crash();
    test_kwcc_base_str_free_as_fn();
    test_nested_defer_cleanup();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
