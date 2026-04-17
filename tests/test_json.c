/*
 * Uracil – JSON parser unit tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "uracil/json.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name)                                                   \
    do {                                                            \
        ++g_tests_run;                                              \
        printf("  %-48s ", #name);                                  \
        name();                                                     \
        ++g_tests_passed;                                           \
        printf("PASS\n");                                           \
    } while (0)

#define ASSERT(condition)                                           \
    do {                                                            \
        if (!(condition)) {                                         \
            printf("FAIL\n    assertion failed: %s\n    at %s:%d\n",\
                   #condition, __FILE__, __LINE__);                 \
            exit(1);                                                \
        }                                                           \
    } while (0)

/* ── Tests ────────────────────────────────────────────────────────────── */

TEST(test_parse_null)
{
    ur_json_node *node = ur_json_parse("null", 4);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_NULL);
    ur_json_free(node);
}

TEST(test_parse_true)
{
    ur_json_node *node = ur_json_parse("true", 4);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_BOOL);
    ASSERT(ur_json_bool(node) == true);
    ur_json_free(node);
}

TEST(test_parse_false)
{
    ur_json_node *node = ur_json_parse("false", 5);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_BOOL);
    ASSERT(ur_json_bool(node) == false);
    ur_json_free(node);
}

TEST(test_parse_integer)
{
    ur_json_node *node = ur_json_parse("42", 2);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_NUMBER);
    ASSERT(fabs(ur_json_number(node) - 42.0) < 0.001);
    ur_json_free(node);
}

TEST(test_parse_negative)
{
    ur_json_node *node = ur_json_parse("-17", 3);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_NUMBER);
    ASSERT(fabs(ur_json_number(node) - (-17.0)) < 0.001);
    ur_json_free(node);
}

TEST(test_parse_float)
{
    ur_json_node *node = ur_json_parse("3.14", 4);
    ASSERT(node != NULL);
    ASSERT(fabs(ur_json_number(node) - 3.14) < 0.001);
    ur_json_free(node);
}

TEST(test_parse_string)
{
    ur_json_node *node = ur_json_parse("\"hello world\"", 13);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_STRING);
    ASSERT(strcmp(ur_json_string(node), "hello world") == 0);
    ur_json_free(node);
}

TEST(test_parse_string_escapes)
{
    const char *input = "\"line\\none\\ttab\"";
    ur_json_node *node = ur_json_parse(input, strlen(input));
    ASSERT(node != NULL);
    ASSERT(strcmp(ur_json_string(node), "line\none\ttab") == 0);
    ur_json_free(node);
}

TEST(test_parse_empty_array)
{
    ur_json_node *node = ur_json_parse("[]", 2);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_ARRAY);
    ASSERT(ur_json_array_length(node) == 0);
    ur_json_free(node);
}

TEST(test_parse_string_array)
{
    const char *input = "[\"fs.read\", \"net.client\", \"clock.read\"]";
    ur_json_node *node = ur_json_parse(input, strlen(input));
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_ARRAY);
    ASSERT(ur_json_array_length(node) == 3);
    ASSERT(strcmp(ur_json_string(ur_json_array_get(node, 0)), "fs.read") == 0);
    ASSERT(strcmp(ur_json_string(ur_json_array_get(node, 1)), "net.client") == 0);
    ASSERT(strcmp(ur_json_string(ur_json_array_get(node, 2)), "clock.read") == 0);
    ur_json_free(node);
}

TEST(test_parse_empty_object)
{
    ur_json_node *node = ur_json_parse("{}", 2);
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_OBJECT);
    ur_json_free(node);
}

TEST(test_parse_simple_object)
{
    const char *input = "{\"name\": \"test\", \"version\": \"1.0\"}";
    ur_json_node *node = ur_json_parse(input, strlen(input));
    ASSERT(node != NULL);
    ASSERT(node->type == UR_JSON_OBJECT);
    ASSERT(strcmp(ur_json_string(ur_json_get(node, "name")), "test") == 0);
    ASSERT(strcmp(ur_json_string(ur_json_get(node, "version")), "1.0") == 0);
    ASSERT(ur_json_get(node, "missing") == NULL);
    ur_json_free(node);
}

TEST(test_parse_nested_object)
{
    const char *input =
        "{"
        "  \"runtime\": {"
        "    \"name\": \"uracil\","
        "    \"abi\": \"uracil.service.v1\""
        "  }"
        "}";
    ur_json_node *node = ur_json_parse(input, strlen(input));
    ASSERT(node != NULL);
    const ur_json_node *runtime = ur_json_get(node, "runtime");
    ASSERT(runtime != NULL);
    ASSERT(runtime->type == UR_JSON_OBJECT);
    ASSERT(strcmp(ur_json_string(ur_json_get(runtime, "name")), "uracil") == 0);
    ASSERT(strcmp(ur_json_string(ur_json_get(runtime, "abi")), "uracil.service.v1") == 0);
    ur_json_free(node);
}

TEST(test_parse_manifest_shaped)
{
    const char *input =
        "{"
        "  \"name\": \"weather-service\","
        "  \"version\": \"0.1.0\","
        "  \"description\": \"Checks weather\","
        "  \"runtime\": {"
        "    \"name\": \"uracil\","
        "    \"abi\": \"uracil.service.v1\","
        "    \"mode\": \"rootless\","
        "    \"hotswap_group\": \"weather-service\","
        "    \"payload_revision\": \"001\""
        "  },"
        "  \"service\": {"
        "    \"entry_function\": \"health\","
        "    \"functions\": ["
        "      {"
        "        \"name\": \"health\","
        "        \"description\": \"Service health\","
        "        \"required_permissions\": []"
        "      },"
        "      {"
        "        \"name\": \"check\","
        "        \"description\": \"Check weather\","
        "        \"required_permissions\": [\"net.client\"]"
        "      }"
        "    ]"
        "  },"
        "  \"permissions\": {"
        "    \"requested\": [\"net.client\"]"
        "  }"
        "}";
    ur_json_node *node = ur_json_parse(input, strlen(input));
    ASSERT(node != NULL);
    ASSERT(strcmp(ur_json_string(ur_json_get(node, "name")), "weather-service") == 0);

    const ur_json_node *service = ur_json_get(node, "service");
    ASSERT(service != NULL);
    const ur_json_node *functions = ur_json_get(service, "functions");
    ASSERT(functions != NULL);
    ASSERT(ur_json_array_length(functions) == 2);

    const ur_json_node *check_fn = ur_json_array_get(functions, 1);
    ASSERT(strcmp(ur_json_string(ur_json_get(check_fn, "name")), "check") == 0);

    const ur_json_node *perms = ur_json_get(check_fn, "required_permissions");
    ASSERT(ur_json_array_length(perms) == 1);
    ASSERT(strcmp(ur_json_string(ur_json_array_get(perms, 0)), "net.client") == 0);

    ur_json_free(node);
}

TEST(test_parse_invalid_returns_null)
{
    ASSERT(ur_json_parse(NULL, 0) == NULL);
    ASSERT(ur_json_parse("", 0) == NULL);
    ASSERT(ur_json_parse("{invalid", 8) == NULL);
    ASSERT(ur_json_parse("[1, 2,]", 7) == NULL);
}

TEST(test_parse_trailing_garbage)
{
    ASSERT(ur_json_parse("42 garbage", 10) == NULL);
}

TEST(test_accessors_on_wrong_types)
{
    ur_json_node *num = ur_json_parse("42", 2);
    ASSERT(ur_json_string(num) == NULL);
    ASSERT(ur_json_array_length(num) == 0);
    ASSERT(ur_json_get(num, "key") == NULL);
    ASSERT(ur_json_bool(num) == false);
    ur_json_free(num);
}

/* ── Runner ───────────────────────────────────────────────────────────── */

int main(void)
{
    printf("JSON parser tests:\n");

    RUN(test_parse_null);
    RUN(test_parse_true);
    RUN(test_parse_false);
    RUN(test_parse_integer);
    RUN(test_parse_negative);
    RUN(test_parse_float);
    RUN(test_parse_string);
    RUN(test_parse_string_escapes);
    RUN(test_parse_empty_array);
    RUN(test_parse_string_array);
    RUN(test_parse_empty_object);
    RUN(test_parse_simple_object);
    RUN(test_parse_nested_object);
    RUN(test_parse_manifest_shaped);
    RUN(test_parse_invalid_returns_null);
    RUN(test_parse_trailing_garbage);
    RUN(test_accessors_on_wrong_types);

    printf("\n%d/%d tests passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
