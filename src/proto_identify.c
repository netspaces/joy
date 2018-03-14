/*
 *
 * Copyright (c) 2018 Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file proto_identify.c
 *
 * \brief Protocol identification (source)
 *
 */

#include <stdlib.h>
#include <string.h>

#include "proto_identify.h"
#include "err.h"


/* Values indicating direction of the flow */
#define DIR_UNKNOWN 0
#define DIR_CLIENT 1
#define DIR_SERVER 2

/**
 * \brief Protocol Inference container
 */
struct pi_container {
    uint8_t dir; /* Flow direction */
    uint16_t app; /* Application protocol prediction */
};

/* --------------------------------------------------
 * --------------------------------------------------
 * KEYWORDS LIST
 * --------------------------------------------------
 * --------------------------------------------------
 */

#define MAX_VAL_LEN 32
#define MAX_VAL_BYTES (2 * MAX_VAL_LEN)

/**
 * \brief Struct that holds the keyword value and inference data.
 */
struct keyword_container {
    uint16_t value[MAX_VAL_LEN]; /**< The keyword value (array of uint16_t) */
    unsigned int value_len; /**< The length of val (number of uint16_t elements) */
    struct pi_container pi; /* Protocol inference struct */
};

#define MAX_KEYWORDS 256
static unsigned int num_keywords = 0;

/**
 * \brief Strings to use for construction of keyword_dict.
 */
static struct keyword_container keywords[MAX_KEYWORDS];

/**
 * \brief Wildcard represents "any" value.
 * Hex = 100
 * Decimal = 256
 */
#define WILDCARD 0x100

/*
 * \brief Add the keyword to the list.
 *
 * \param[in] value Pointer to array of uint16_t values
 * \param[in] value_bytes_len Length of value array in bytes
 * \param[in] pi Pointer to the struct holding protocol inference
 *
 * \return 0 for success, 1 for failure
 */
static int add_keyword(const uint16_t *value,
                       unsigned int value_bytes_len,
                       const struct pi_container *pi) {

    struct keyword_container *kc = NULL;

    if (num_keywords >= MAX_KEYWORDS) {
        joy_log_err("no more slots for keyword");
        return 1;
    }

    if (value_bytes_len > MAX_VAL_BYTES) {
        joy_log_err("value_bytes_len (%d) > MAX_VAL_BYTES (%d)",
                    value_bytes_len, MAX_VAL_BYTES);
        return 1;
    }

    /*
     * Get latest position of keyword list
     */
    kc = &keywords[num_keywords];

    /* Copy the value array */
    memcpy(kc->value, value, value_bytes_len);
    /* Convert from byte length to array length */
    kc->value_len = (value_bytes_len / sizeof(uint16_t));
    /* Copy the protocol inference struct */
    memcpy(&kc->pi, pi, sizeof(struct pi_container));

    /*
     * Increment the count of keywords ingested
     */
    num_keywords++;

    return 0;
}

/*
 * \brief Add TLS keyword identifiers.
 *
 * \param none
 *
 * \return 0 for success, 1 for failure
 */
static int add_tls_identifiers(void) {
    struct pi_container pi;

    {
        /* TEMPLATE adding a keyword */
        uint16_t string[] = {0x16, 0x03, 0x01, WILDCARD, WILDCARD, 0x01};
        pi.app = 443;
        pi.dir = DIR_CLIENT;

        if (add_keyword(string, sizeof(string), &pi)) {
            joy_log_err("problem adding keyword");
            return 1;
        }
    }

    {
        uint16_t string[] = {0x16, 0x03, 0x01, WILDCARD, WILDCARD, 0x02};
        pi.app = 443;
        pi.dir = DIR_SERVER;

        if (add_keyword(string, sizeof(string), &pi)) {
            joy_log_err("problem adding keyword");
            return 1;
        }
    }


    return 0;
}

/*
 * \brief Add all protocol keyword identifiers.
 *
 * \param none
 *
 * \return 0 for success, 1 for failure
 */
static int populate_keyword_identifiers(void) {
    if (add_tls_identifiers()) {
        joy_log_err("problem populating tls keywords");
        return 1;
    }

    return 0;
}

/*
 * \brief Initialize and setup the keywords list.
 *
 * \param none
 *
 * \return 0 for success, 1 for failure
 */
static int init_keywords(void) {
    /* Zero out the keywords array */
    memset(&keywords, 0, sizeof(keywords));

    /* Populate the keywords array */
    if(populate_keyword_identifiers()) {
        return 1;
    }

    return 0;
}

/* --------------------------------------------------
 * --------------------------------------------------
 * KEYWORD DICTIONARY MATCHING
 * --------------------------------------------------
 * --------------------------------------------------
 */

#define MAX_CHILDREN 257 /**< Full range of 1 byte, plus an extra slot for "wildcard" */
/**
 * \brief Node of the keyword_dict tree.
 */
struct keyword_dict_node {
    uint16_t edge[MAX_CHILDREN]; /**< 16bit value, so that it can hold "wildcard"
                                      in addition to full-range of single byte (0-255) */
    struct keyword_dict_node *child; /* Pointer to first child */
    struct keyword_dict_node *sibling; /* Pointer to next sibling */
    uint16_t num_children; /* Number of children under this node */
    struct pi_container pi; /* Protocol inference */
};

/*
 * Root of the keyword_dict tree
 */
struct keyword_dict_node *kd_root = NULL;

/**
 * \brief Allocate memory for a keyword_dict_node
 *
 * \param none
 *
 * \return Pointer to newly allocated keyword_dict_node
 */
static struct keyword_dict_node *alloc_kdn(void) {

    return calloc(1, sizeof(struct keyword_dict_node));
}

/**
 * \brief Free the node, and all children nodes underneath.
 *
 * \param[in] node Pointer to the root keyword_dict_node
 *
 * \return none
 */
static void destroy_kdn(struct keyword_dict_node *node) {

    struct keyword_dict_node *child = NULL;
    struct keyword_dict_node *sibling = NULL;

    if (node == NULL) {
        return;
    }

    if (node->child == NULL || node->num_children == 0) {
        free(node);
        return;
    }

    /* The first child */
    child = node->child;

    while (child != NULL) {
        /* Get the next child node before deleting this one */
        sibling = child->sibling;

        /* Recurse */
        destroy_kdn(child);

        /* Tee up the next one */
        child = sibling;
    }

    /* Now delete the parent */
    free(node);
    return;
}

/**
 * \brief Create a child node under the parent \p node.
 *
 * This function should only be used to generate the first child.
 * Use kdn_create_sibling for making more children.
 *
 * \param[in] parent Pointer to the parent node
 * \param[in] edge Integer value representing the child node
 *
 * \return Pointer to child, or NULL
 */
static struct keyword_dict_node *kdn_create_child(struct keyword_dict_node *parent,
                                                  uint16_t edge) {
    if (parent == NULL) {
        return NULL;
    }

    if (parent->child || (parent->num_children != 0)) {
        joy_log_err("child already exists");
        return NULL;
    }

    parent->child = alloc_kdn();

    /* Set the edge value corresponding to this child */
    parent->edge[parent->num_children] = edge;

    /* Increment household count */
    parent->num_children++;

    return parent->child;
}

/**
 * \brief Create a sibling node, parallel to the \p child, under the \p parent.
 *
 * \param[in] parent Pointer to the parent node
 * \param[in] child Pointer to the child node
 * \param[in] edge Integer value representing the sibling node
 *
 * \return Pointer to sibling, or NULL
 */
static struct keyword_dict_node *kdn_create_sibling(struct keyword_dict_node *parent,
                                                    struct keyword_dict_node *child,
                                                    uint16_t edge) {
    if (parent == NULL || child == NULL) {
        return NULL;
    }

    if (child->sibling != NULL) {
        joy_log_err("child already has a sibling");
    }

    if (parent->num_children == (MAX_CHILDREN - 1)) {
        joy_log_err("already at MAX_CHILDREN(%d)", MAX_CHILDREN);
        return NULL;
    }

    /* The first one! */
    child->sibling = alloc_kdn();

    /* Set the edge value corresponding to this child */
    parent->edge[parent->num_children] = edge;

    /* Increment household count */
    parent->num_children++;

    return child->sibling;
}

/**
 * \brief Add a keyword to the keyword_dictionary tree.
 *
 * \param[in] root Pointer to the root node of the dictionary tree
 * \param[in] kc Pointer to the keyword_container, representing the keyword
 *
 * \return 0 for success, 1 for failure
 */
static int keyword_dict_add_keyword(struct keyword_dict_node *root,
                                    const struct keyword_container *kc) {

    struct keyword_dict_node *node = NULL;
    int i = 0;

    if (root == NULL || kc == NULL) {
        return 0;
    }

    /* The root node of the tree */
    node = root;

    for (i = 0; i < kc->value_len; i++) {
        /* Grab the next value of string for comparison */
        uint16_t val = *(kc->value + i);

        if (node->child) {
            struct keyword_dict_node *child = node->child;
            int match = 0;
            int k = 0;

            for (k = 0; k < node->num_children; k++) {
                /*
                 * Compare this character to all the children edges
                 */
                if (val == node->edge[k]) {
                    /* Matches the edge to this child */
                    match = 1;
                    break;
                }

                if (child->sibling != NULL) {
                    /* Point to the next child */
                    child = child->sibling;
                }
            }

            if (match) {
                node = child;
            } else {
                /* Make a new sibling node/edge */
                node = kdn_create_sibling(node, child, val);
            }
        } else {
            /*
             * No existing children.
             * Make a new child node/edge.
             */
            node = kdn_create_child(node, val);
        }
    }

    /*
     * This is the end node for the keyword.
     * Set the protocol inference information.
     */
    node->pi = kc->pi;

    return 0;
}

/**
 * \brief Construct the keyword dictionary tree, adding all keywords in the list.
 *
 * \param[in] root Handle to the root node of the dictionary tree
 *
 * \return 0 for success, 1 for failure
 */
static int construct_keyword_dict(struct keyword_dict_node **root) {

    int i = 0;

    if (root == NULL || *root != NULL) {
        return 1;
    }

    *root = alloc_kdn();

    /*
     * Looping over the list of keywords,
     * construct the keyword_dictionary tree.
     */
    for (i = 0; i < num_keywords; i++) {
        /* Pointer to the latest keyword */
        struct keyword_container *kc = &keywords[i];

        if (keyword_dict_add_keyword(*root, kc)) {
            joy_log_err("couldn't add keywords[%d]", i);
        }
    }

    return 0;
}

/**
 * \brief Search the tree, beginning at \p root, for any keywords that match the \p data.
 *
 * \param[in] root Pointer to the root node of the dictionary tree
 * \param[in] data Pointer to the data
 * \param[in] data_len Length of the data in bytes
 *
 * \return Pointer to protocol inference container, or NULL
 */
static const struct pi_container *search_keyword_dict(const struct keyword_dict_node *root,
                                                      const char *data,
                                                      unsigned int data_len) {

    const struct keyword_dict_node *node = root;
    const struct keyword_dict_node *child = NULL;
    int i = 0;

    if (root == NULL || data == NULL || data_len == 0) {
        return NULL;
    }

    /* The first child (may be null) */
    child = node->child;

    /*
     * Check to see if any children are an "end" node.
     * If no children exist, this loop will be skipped.
     */
    for (i = 0; i < node->num_children; i++) {
        if ((node->edge[i] == WILDCARD) || (*data == node->edge[i])) {
            if (child->pi.app) {
                /*
                 * Match!
                 * We've reached a node that contains
                 * protocol inference information.
                 */
                return &child->pi;
            } else {
                /*
                 * Compare next layer node
                 */
                if (data_len == 1) {
                    /* No more data to compare */
                    return NULL;
                }

                return search_keyword_dict(child, (data + 1), (data_len - 1));
            }
        }

        /* Get the next child */
        child = child->sibling;
    }

    /* No more children */
    return NULL;
}

/**
 * \brief Initialize and setup the proto_identify keyword dictionary.
 *
 * \param none
 *
 * \return 0 for success, 1 for failure
 */
int proto_identify_init_keyword_dict(void) {
    if (kd_root != NULL) {
        return 1;
    }

    if (num_keywords == 0) {
        /* Initialize the list of keywords */
        if (init_keywords()) {
            joy_log_err("failed to initialize keyword list");
            return 1;
        }
    }

    /* Create the tree graph */
    construct_keyword_dict(&kd_root);

    return 0;
}

/**
 * \brief Teardown the proto_identify keyword dictionary, and all associated memory.
 *
 * \param none
 *
 * \return none
 */
void proto_identify_destroy_keyword_dict(void) {
    if (kd_root == NULL) {
        return;
    }

    destroy_kdn(kd_root);
}

uint16_t identify_tcp_protocol(const char *tcp_data, unsigned int len) {

    uint16_t port = 0;
    const struct pi_container *pi = NULL;

    if (len == 0) {
        return 0;
    }

    if (kd_root == NULL) {
        proto_identify_init_keyword_dict();
    }

    pi = search_keyword_dict(kd_root, tcp_data, len) ;
    if (pi != NULL) {
        port = pi->app;
    }

    return port;
}

