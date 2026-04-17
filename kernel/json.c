/*
 * Uracil – Minimal recursive-descent JSON parser.
 *
 * Handles the subset of JSON needed for service manifests:
 * objects, arrays, strings, numbers, booleans, and null.
 *
 * The parser operates on a read-only input buffer and produces
 * a tree of ur_json_node values.  All memory is heap-allocated;
 * ur_json_free() releases the entire tree.
 */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "uracil/json.h"

/* ── Scanner state ────────────────────────────────────────────────────── */

typedef struct ur_json_scanner {
    const char *data;
    size_t length;
    size_t position;
} ur_json_scanner;

static void ur_json_skip_whitespace(ur_json_scanner *scanner)
{
    while ((scanner->position < scanner->length) &&
           isspace((unsigned char)scanner->data[scanner->position])) {
        ++scanner->position;
    }
}

static char ur_json_peek(const ur_json_scanner *scanner)
{
    if (scanner->position >= scanner->length) {
        return '\0';
    }
    return scanner->data[scanner->position];
}

static char ur_json_advance(ur_json_scanner *scanner)
{
    if (scanner->position >= scanner->length) {
        return '\0';
    }
    return scanner->data[scanner->position++];
}

static bool ur_json_match(ur_json_scanner *scanner, char expected)
{
    ur_json_skip_whitespace(scanner);
    if (ur_json_peek(scanner) == expected) {
        ++scanner->position;
        return true;
    }
    return false;
}

static bool ur_json_match_literal(ur_json_scanner *scanner, const char *literal, size_t len)
{
    if ((scanner->position + len) > scanner->length) {
        return false;
    }
    if (memcmp(scanner->data + scanner->position, literal, len) != 0) {
        return false;
    }
    scanner->position += len;
    return true;
}

/* ── Node allocation ──────────────────────────────────────────────────── */

static ur_json_node *ur_json_node_alloc(ur_json_type type)
{
    ur_json_node *node = calloc(1U, sizeof(*node));
    if (node != NULL) {
        node->type = type;
    }
    return node;
}

/* ── Forward declarations ─────────────────────────────────────────────── */

static ur_json_node *ur_json_parse_value(ur_json_scanner *scanner);

/* ── String parsing ───────────────────────────────────────────────────── */

static char *ur_json_parse_string_raw(ur_json_scanner *scanner)
{
    size_t start;
    size_t capacity = 64U;
    size_t used = 0U;
    char *buffer;

    if (ur_json_advance(scanner) != '"') {
        return NULL;
    }

    buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }

    start = scanner->position;
    UR_UNUSED(start);

    while (scanner->position < scanner->length) {
        char current = scanner->data[scanner->position++];

        if (current == '"') {
            buffer[used] = '\0';
            return buffer;
        }

        if (current == '\\') {
            if (scanner->position >= scanner->length) {
                break;
            }
            current = scanner->data[scanner->position++];
            switch (current) {
                case '"':  current = '"';  break;
                case '\\': current = '\\'; break;
                case '/':  current = '/';  break;
                case 'b':  current = '\b'; break;
                case 'f':  current = '\f'; break;
                case 'n':  current = '\n'; break;
                case 'r':  current = '\r'; break;
                case 't':  current = '\t'; break;
                case 'u':
                    /* Skip 4 hex digits (store as '?') */
                    if ((scanner->position + 4U) <= scanner->length) {
                        scanner->position += 4U;
                    }
                    current = '?';
                    break;
                default:
                    break;
            }
        }

        if (used + 1U >= capacity) {
            capacity *= 2U;
            char *resized = realloc(buffer, capacity);
            if (resized == NULL) {
                free(buffer);
                return NULL;
            }
            buffer = resized;
        }

        buffer[used++] = current;
    }

    free(buffer);
    return NULL;
}

static ur_json_node *ur_json_parse_string(ur_json_scanner *scanner)
{
    char *text = ur_json_parse_string_raw(scanner);
    ur_json_node *node;

    if (text == NULL) {
        return NULL;
    }

    node = ur_json_node_alloc(UR_JSON_STRING);
    if (node == NULL) {
        free(text);
        return NULL;
    }

    node->value.string = text;
    return node;
}

/* ── Number parsing ───────────────────────────────────────────────────── */

static ur_json_node *ur_json_parse_number(ur_json_scanner *scanner)
{
    const char *start = scanner->data + scanner->position;
    char *end_ptr = NULL;
    double result;
    ur_json_node *node;

    result = strtod(start, &end_ptr);
    if (end_ptr == start) {
        return NULL;
    }

    scanner->position += (size_t)(end_ptr - start);

    node = ur_json_node_alloc(UR_JSON_NUMBER);
    if (node == NULL) {
        return NULL;
    }

    node->value.number = result;
    return node;
}

/* ── Array parsing ────────────────────────────────────────────────────── */

static ur_json_node *ur_json_parse_array(ur_json_scanner *scanner)
{
    ur_json_node *node;
    size_t capacity = 8U;

    /* Consume '[' */
    ++scanner->position;

    node = ur_json_node_alloc(UR_JSON_ARRAY);
    if (node == NULL) {
        return NULL;
    }

    node->value.array.items = calloc(capacity, sizeof(ur_json_node *));
    if (node->value.array.items == NULL) {
        free(node);
        return NULL;
    }

    ur_json_skip_whitespace(scanner);
    if (ur_json_peek(scanner) == ']') {
        ++scanner->position;
        return node;
    }

    for (;;) {
        ur_json_node *item = ur_json_parse_value(scanner);
        if (item == NULL) {
            ur_json_free(node);
            return NULL;
        }

        if (node->value.array.count >= capacity) {
            capacity *= 2U;
            ur_json_node **resized = realloc(
                node->value.array.items,
                capacity * sizeof(ur_json_node *)
            );
            if (resized == NULL) {
                ur_json_free(item);
                ur_json_free(node);
                return NULL;
            }
            node->value.array.items = resized;
        }

        node->value.array.items[node->value.array.count++] = item;

        if (ur_json_match(scanner, ',')) {
            continue;
        }

        if (ur_json_match(scanner, ']')) {
            return node;
        }

        ur_json_free(node);
        return NULL;
    }
}

/* ── Object parsing ───────────────────────────────────────────────────── */

static ur_json_node *ur_json_parse_object(ur_json_scanner *scanner)
{
    ur_json_node *node;
    size_t capacity = 8U;

    /* Consume '{' */
    ++scanner->position;

    node = ur_json_node_alloc(UR_JSON_OBJECT);
    if (node == NULL) {
        return NULL;
    }

    node->value.object.members = calloc(capacity, sizeof(ur_json_node *));
    if (node->value.object.members == NULL) {
        free(node);
        return NULL;
    }

    ur_json_skip_whitespace(scanner);
    if (ur_json_peek(scanner) == '}') {
        ++scanner->position;
        return node;
    }

    for (;;) {
        char *key;
        ur_json_node *member;

        ur_json_skip_whitespace(scanner);
        key = ur_json_parse_string_raw(scanner);
        if (key == NULL) {
            ur_json_free(node);
            return NULL;
        }

        if (!ur_json_match(scanner, ':')) {
            free(key);
            ur_json_free(node);
            return NULL;
        }

        member = ur_json_parse_value(scanner);
        if (member == NULL) {
            free(key);
            ur_json_free(node);
            return NULL;
        }

        member->key = key;

        if (node->value.object.count >= capacity) {
            capacity *= 2U;
            ur_json_node **resized = realloc(
                node->value.object.members,
                capacity * sizeof(ur_json_node *)
            );
            if (resized == NULL) {
                ur_json_free(member);
                ur_json_free(node);
                return NULL;
            }
            node->value.object.members = resized;
        }

        node->value.object.members[node->value.object.count++] = member;

        if (ur_json_match(scanner, ',')) {
            continue;
        }

        if (ur_json_match(scanner, '}')) {
            return node;
        }

        ur_json_free(node);
        return NULL;
    }
}

/* ── Value parsing (dispatch) ─────────────────────────────────────────── */

static ur_json_node *ur_json_parse_value(ur_json_scanner *scanner)
{
    char current;

    ur_json_skip_whitespace(scanner);
    current = ur_json_peek(scanner);

    if (current == '"') {
        return ur_json_parse_string(scanner);
    }

    if (current == '{') {
        return ur_json_parse_object(scanner);
    }

    if (current == '[') {
        return ur_json_parse_array(scanner);
    }

    if ((current == '-') || ((current >= '0') && (current <= '9'))) {
        return ur_json_parse_number(scanner);
    }

    if (ur_json_match_literal(scanner, "true", 4U)) {
        ur_json_node *node = ur_json_node_alloc(UR_JSON_BOOL);
        if (node != NULL) {
            node->value.boolean = true;
        }
        return node;
    }

    if (ur_json_match_literal(scanner, "false", 5U)) {
        ur_json_node *node = ur_json_node_alloc(UR_JSON_BOOL);
        if (node != NULL) {
            node->value.boolean = false;
        }
        return node;
    }

    if (ur_json_match_literal(scanner, "null", 4U)) {
        return ur_json_node_alloc(UR_JSON_NULL);
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

ur_json_node *ur_json_parse(const char *text, size_t length)
{
    ur_json_scanner scanner;
    ur_json_node *root;

    if ((text == NULL) || (length == 0U)) {
        return NULL;
    }

    scanner.data = text;
    scanner.length = length;
    scanner.position = 0U;

    root = ur_json_parse_value(&scanner);

    /* Ensure no trailing garbage (except whitespace) */
    if (root != NULL) {
        ur_json_skip_whitespace(&scanner);
        if (scanner.position != scanner.length) {
            ur_json_free(root);
            return NULL;
        }
    }

    return root;
}

void ur_json_free(ur_json_node *node)
{
    size_t index;

    if (node == NULL) {
        return;
    }

    free(node->key);

    switch (node->type) {
        case UR_JSON_STRING:
            free(node->value.string);
            break;

        case UR_JSON_ARRAY:
            for (index = 0U; index < node->value.array.count; ++index) {
                ur_json_free(node->value.array.items[index]);
            }
            free(node->value.array.items);
            break;

        case UR_JSON_OBJECT:
            for (index = 0U; index < node->value.object.count; ++index) {
                ur_json_free(node->value.object.members[index]);
            }
            free(node->value.object.members);
            break;

        default:
            break;
    }

    free(node);
}

const ur_json_node *ur_json_get(const ur_json_node *object, const char *key)
{
    size_t index;

    if ((object == NULL) || (key == NULL) || (object->type != UR_JSON_OBJECT)) {
        return NULL;
    }

    for (index = 0U; index < object->value.object.count; ++index) {
        const ur_json_node *member = object->value.object.members[index];
        if ((member->key != NULL) && (strcmp(member->key, key) == 0)) {
            return member;
        }
    }

    return NULL;
}

const char *ur_json_string(const ur_json_node *node)
{
    if ((node == NULL) || (node->type != UR_JSON_STRING)) {
        return NULL;
    }
    return node->value.string;
}

double ur_json_number(const ur_json_node *node)
{
    if ((node == NULL) || (node->type != UR_JSON_NUMBER)) {
        return 0.0;
    }
    return node->value.number;
}

bool ur_json_bool(const ur_json_node *node)
{
    if ((node == NULL) || (node->type != UR_JSON_BOOL)) {
        return false;
    }
    return node->value.boolean;
}

size_t ur_json_array_length(const ur_json_node *node)
{
    if ((node == NULL) || (node->type != UR_JSON_ARRAY)) {
        return 0U;
    }
    return node->value.array.count;
}

const ur_json_node *ur_json_array_get(const ur_json_node *node, size_t index)
{
    if ((node == NULL) || (node->type != UR_JSON_ARRAY) ||
        (index >= node->value.array.count)) {
        return NULL;
    }
    return node->value.array.items[index];
}
