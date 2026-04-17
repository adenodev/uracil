#ifndef URACIL_JSON_H
#define URACIL_JSON_H

#include "uracil/common.h"

typedef enum ur_json_type {
    UR_JSON_NULL = 0,
    UR_JSON_BOOL,
    UR_JSON_NUMBER,
    UR_JSON_STRING,
    UR_JSON_ARRAY,
    UR_JSON_OBJECT
} ur_json_type;

typedef struct ur_json_node ur_json_node;

struct ur_json_node {
    ur_json_type type;
    char *key;              /* non-NULL for object members */

    union {
        bool boolean;
        double number;
        char *string;
        struct {
            ur_json_node **items;
            size_t count;
        } array;
        struct {
            ur_json_node **members;
            size_t count;
        } object;
    } value;
};

/**
 * Parse a JSON string into a tree of nodes.
 * Returns NULL on parse error.
 * The caller must free the result with ur_json_free().
 */
ur_json_node *ur_json_parse(const char *text, size_t length);

/**
 * Free a JSON node tree.
 */
void ur_json_free(ur_json_node *node);

/**
 * Look up a member in a JSON object by key.
 * Returns NULL if the node is not an object or the key is not found.
 */
const ur_json_node *ur_json_get(const ur_json_node *object, const char *key);

/**
 * Get the string value of a JSON node.
 * Returns NULL if the node is not a string.
 */
const char *ur_json_string(const ur_json_node *node);

/**
 * Get the number value of a JSON node.
 * Returns 0.0 if the node is not a number.
 */
double ur_json_number(const ur_json_node *node);

/**
 * Get the boolean value of a JSON node.
 * Returns false if the node is not a boolean.
 */
bool ur_json_bool(const ur_json_node *node);

/**
 * Get the array length of a JSON node.
 * Returns 0 if the node is not an array.
 */
size_t ur_json_array_length(const ur_json_node *node);

/**
 * Get an array element by index.
 * Returns NULL if the node is not an array or the index is out of bounds.
 */
const ur_json_node *ur_json_array_get(const ur_json_node *node, size_t index);

#endif
