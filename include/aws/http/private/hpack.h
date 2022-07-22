#ifndef AWS_HTTP_HPACK_H
#define AWS_HTTP_HPACK_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/request_response.h>
/* #TODO probably need lets more includes */

/* #TODO test empty strings */

struct aws_hpack_dynamic_table {
    /* Array of headers, pointers to memory we alloced, which needs to be cleaned up whenever we move an entry out */
    struct aws_http_header *buffer;
    size_t buffer_capacity; /* Number of http_headers that can fit in buffer */

    size_t num_elements;
    size_t index_0;

    /* Size in bytes, according to [4.1] */
    size_t size;
    size_t max_size;

    /* TODO: check the new (RFC 9113 - 4.3.1) to make sure we did it right */
    /* SETTINGS_HEADER_TABLE_SIZE from http2 */
    size_t protocol_max_size_setting;
    /* aws_http_header * -> size_t */
    struct aws_hash_table reverse_lookup;
    /* aws_byte_cursor * -> size_t */
    struct aws_hash_table reverse_lookup_name_only;
};

/**
 * Result of aws_hpack_decode() call.
 * If a complete entry has not been decoded yet, type is ONGOING.
 * Otherwise, type informs which data to look at.
 */
struct aws_hpack_decode_result {
    enum aws_hpack_decode_type {
        AWS_HPACK_DECODE_T_ONGOING,
        AWS_HPACK_DECODE_T_HEADER_FIELD,
        AWS_HPACK_DECODE_T_DYNAMIC_TABLE_RESIZE,
    } type;

    union {
        /* If type is AWS_HPACK_DECODE_T_HEADER_FIELD */
        struct aws_http_header header_field;

        /* If type is AWS_HPACK_DECODE_T_DYNAMIC_TABLE_RESIZE */
        size_t dynamic_table_resize;
    } data;
};

/**
 * Controls whether non-indexed strings will use Huffman encoding.
 * In SMALLEST mode, strings will only be sent with Huffman encoding if it makes them smaller.
 *
 * Note: This does not control compression via "indexing",
 * for that, see `aws_http_header_compression`.
 * This only controls how string values are encoded when they're not already in a table.
 */
enum aws_hpack_huffman_mode {
    AWS_HPACK_HUFFMAN_SMALLEST,
    AWS_HPACK_HUFFMAN_NEVER,
    AWS_HPACK_HUFFMAN_ALWAYS,
};

struct aws_hpack_encoder {
    const void *log_id;

    struct aws_huffman_encoder huffman_encoder;
    enum aws_hpack_huffman_mode huffman_mode;

    struct aws_hpack_dynamic_table dynamic_table;

    struct {
        size_t last_value;
        size_t smallest_value;
        bool pending;
    } dynamic_table_size_update;
};

struct aws_hpack_decoder {
    const void *log_id;

    struct aws_huffman_decoder huffman_decoder;
    struct aws_hpack_dynamic_table dynamic_table;

    /* PRO TIP: Don't union progress_integer and progress_string together, since string_decode calls integer_decode */
    struct hpack_progress_integer {
        enum {
            HPACK_INTEGER_STATE_INIT,
            HPACK_INTEGER_STATE_VALUE,
        } state;
        uint8_t bit_count;
    } progress_integer;

    struct hpack_progress_string {
        enum {
            HPACK_STRING_STATE_INIT,
            HPACK_STRING_STATE_LENGTH,
            HPACK_STRING_STATE_VALUE,
        } state;
        bool use_huffman;
        uint64_t length;
    } progress_string;

    struct hpack_progress_entry {
        enum {
            HPACK_ENTRY_STATE_INIT,
            /* Indexed header field: just 1 state. read index, find name and value at index */
            HPACK_ENTRY_STATE_INDEXED,
            /* Literal header field: name may be indexed OR literal, value is always literal */
            HPACK_ENTRY_STATE_LITERAL_BEGIN,
            HPACK_ENTRY_STATE_LITERAL_NAME_STRING,
            HPACK_ENTRY_STATE_LITERAL_VALUE_STRING,
            /* Dynamic table resize: just 1 state. read new size */
            HPACK_ENTRY_STATE_DYNAMIC_TABLE_RESIZE,
            /* Done */
            HPACK_ENTRY_STATE_COMPLETE,
        } state;

        union {
            struct {
                uint64_t index;
            } indexed;

            struct hpack_progress_literal {
                uint8_t prefix_size;
                enum aws_http_header_compression compression;
                uint64_t name_index;
                size_t name_length;
            } literal;

            struct {
                uint64_t size;
            } dynamic_table_resize;
        } u;

        enum aws_hpack_decode_type type;

        /* Scratch holds header name and value while decoding */
        struct aws_byte_buf scratch;
    } progress_entry;
};

AWS_EXTERN_C_BEGIN

/*******************************************************************************
 * STATIC TABLE
 ******************************************************************************/

/* Library-level init and shutdown */
void aws_hpack_static_table_init(struct aws_allocator *allocator);
void aws_hpack_static_table_clean_up(void);

size_t aws_hpack_static_table_find_name_and_value(const struct aws_http_header *header);
size_t aws_hpack_static_table_find_name_only(struct aws_byte_cursor name);

extern const size_t aws_hpack_static_table_num_elements;

/*******************************************************************************
 * DYNAMIC TABLE
 ******************************************************************************/

AWS_HTTP_API
void aws_hpack_dynamic_table_init(
    struct aws_hpack_dynamic_table *table,
    struct aws_allocator *allocator,
    const void *log_id);

AWS_HTTP_API
void aws_hpack_dynamic_table_clean_up(struct aws_hpack_dynamic_table *table);

/* Returns the number of elements in dynamic table now */
AWS_HTTP_API
size_t aws_hpack_dynamic_table_get_num_elements(const struct aws_hpack_dynamic_table *table);

AWS_HTTP_API
int aws_hpack_dynamic_table_insert_header(struct aws_hpack_dynamic_table *table, const struct aws_http_header *header);

size_t aws_hpack_dynamic_table_find_name_and_value(
    const struct aws_hpack_dynamic_table *table,
    const struct aws_http_header *header,
    bool *out_has_value);

size_t aws_hpack_dynamic_table_find_name_only(const struct aws_hpack_dynamic_table *table, struct aws_byte_cursor name);

/**
 * Set the max size of the dynamic table (in octets). The size of each header is name.len + value.len + 32 [4.1].
 */
AWS_HTTP_API
int aws_hpack_dynamic_table_resize(struct aws_hpack_dynamic_table *table, size_t new_max_size);

/*******************************************************************************
 * ENCODER
 ******************************************************************************/

AWS_HTTP_API
void aws_hpack_encoder_init(struct aws_hpack_encoder *encoder, struct aws_allocator *allocator, const void *log_id);

AWS_HTTP_API
void aws_hpack_encoder_clean_up(struct aws_hpack_encoder *encoder);

AWS_HTTP_API
void aws_hpack_encoder_set_huffman_mode(struct aws_hpack_encoder *encoder, enum aws_hpack_huffman_mode mode);

/* Call this after receiving SETTINGS_HEADER_TABLE_SIZE from peer and sending the ACK.
 * The hpack-encoder remembers all size updates, and makes sure to encode the proper
 * number of Dynamic Table Size Updates the next time a header block is sent. */
AWS_HTTP_API
void aws_hpack_encoder_update_max_table_size(struct aws_hpack_context *context, uint32_t new_max_size);

/**
 * Encode header-block into the output.
 * This function will mutate the hpack context, so an error means the context can no longer be used.
 * Note that output will be dynamically resized if it's too short.
 */
AWS_HTTP_API
int aws_hpack_encode_header_block(
    struct aws_hpack_context *context,
    const struct aws_http_headers *headers,
    struct aws_byte_buf *output);

/* Private part of encoder API, but public for testing purposes.
 * Output will be dynamically resized if it's too short */
AWS_HTTP_API
int aws_hpack_encode_integer(uint64_t integer, uint8_t starting_bits, uint8_t prefix_size, struct aws_byte_buf *output);

/* Private part of encoder API, but public for testing purposes.
 * Output will be dynamically resized if it's too short */
AWS_HTTP_API
int aws_hpack_encode_string(
    struct aws_hpack_context *context,
    struct aws_byte_cursor to_encode,
    struct aws_byte_buf *output);

/* Private part of encoder API, but public for testing purposes.
 * A return value of 0 indicates that the header wasn't found */
AWS_HTTP_API
size_t aws_hpack_encoder_find_index(
    const struct aws_hpack_context *context,
    const struct aws_http_header *header,
    bool search_value /* TODO: do we really need `bool search_value`? it's always true */,
    bool *found_value);

/*******************************************************************************
 * DECODER
 ******************************************************************************/

AWS_HTTP_API
void aws_hpack_decoder_init(struct aws_hpack_decoder *decoder, struct aws_allocator *allocator, const void *log_id);

AWS_HTTP_API
void aws_hpack_decoder_clean_up(struct aws_hpack_decoder *decoder);

/* Call this after sending SETTINGS_HEADER_TABLE_SIZE and receiving ACK from the peer.
 * The hpack-decoder remembers all size updates, and makes sure that the peer
 * sends the appropriate Dynamic Table Size Updates in the next header block we receive. */
AWS_HTTP_API
void aws_hpack_decoder_update_max_table_size(struct aws_hpack_context *context, uint32_t new_max_size);

/**
 * Decode the next entry in the header-block-fragment.
 * If result->type is ONGOING, then call decode() again with more data to resume decoding.
 * Otherwise, type is either a HEADER_FIELD or a DYNAMIC_TABLE_RESIZE.
 *
 * If an error occurs, the decoder is broken and decode() must not be called again.
 */
AWS_HTTP_API
int aws_hpack_decode(
    struct aws_hpack_context *context,
    struct aws_byte_cursor *to_decode,
    struct aws_hpack_decode_result *result);

/* Private part of decoder API, but public for testing purposes. */
AWS_HTTP_API
int aws_hpack_decode_integer(
    struct aws_hpack_context *context,
    struct aws_byte_cursor *to_decode,
    uint8_t prefix_size,
    uint64_t *integer,
    bool *complete);

/* Private part of decoder API, but public for testing purposes. */
AWS_HTTP_API
int aws_hpack_decode_string(
    struct aws_hpack_context *context,
    struct aws_byte_cursor *to_decode,
    struct aws_byte_buf *output,
    bool *complete);

/* Private part of decoder API, but public for testing purposes. */
AWS_HTTP_API
const struct aws_http_header *aws_hpack_decoder_get_header(const struct aws_hpack_context *context, size_t index);

/*******************************************************************************
 * UTILS
 ******************************************************************************/

/* Returns the hpack size of a header (name.len + value.len + 32) [4.1] */
AWS_HTTP_API
size_t aws_hpack_get_header_size(const struct aws_http_header *header);

/* For hash-tables storing headers */
size_t aws_hpack_header_table_hash(const void *key);
bool aws_hpack_header_table_eq(const void *a, const void *b);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_HPACK_H */
