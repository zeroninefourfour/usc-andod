/* Minimal stub of the Backbeat C SDK for platforms without a prebuilt SDK
 * (e.g. Android). All entry points report "unsupported" so the game runs
 * without Backbeat bundle support. Field layout matches how the game code
 * consumes the real SDK. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bkb_error_code
{
	BKB_OK = 0,
	BKB_ERR_UNSUPPORTED = 1,
	BKB_ERR_GENERIC = 2
} bkb_error_code;

/* borrowed string view */
typedef struct bkb_str
{
	const char* ptr;
	size_t len;
} bkb_str;

/* owned string, released with bkb_string_free */
typedef struct bkb_string
{
	const char* ptr;
	size_t len;
} bkb_string;

typedef struct bkb_bytes
{
	const uint8_t* ptr;
	size_t len;
} bkb_bytes;

typedef struct bkb_timestamp
{
	int64_t seconds;
	int32_t nanos;
} bkb_timestamp;

typedef enum bkb_asset_data_kind
{
	BKB_ASSET_DATA_NONE = 0,
	BKB_ASSET_DATA_FILE = 1,
	BKB_ASSET_DATA_BYTES = 2
} bkb_asset_data_kind;

typedef struct bkb_asset_data
{
	bkb_asset_data_kind kind;
	union
	{
		bkb_bytes bytes;
		bkb_string file;
	} value;
} bkb_asset_data;

typedef struct bkb_store bkb_store;

typedef struct bkb_bb
{
	const uint8_t* chart;
	size_t chart_len;
} bkb_bb;

typedef struct bkb_table_chart
{
	bkb_str bundle_id;
	bkb_str chart_id;
} bkb_table_chart;

typedef struct bkb_table_level
{
	bkb_str level;
	const bkb_table_chart* charts;
	size_t charts_len;
} bkb_table_level;

typedef struct bkb_table_folder
{
	bkb_str name;
	const bkb_table_chart* charts;
	size_t charts_len;
} bkb_table_folder;

typedef struct bkb_table
{
	bkb_str name;
	bkb_str symbol;
	const bkb_table_level* levels;
	size_t levels_len;
	const bkb_table_folder* folders;
	size_t folders_len;
} bkb_table;

typedef struct bkb_course_tag
{
	bkb_str key;
	bkb_str value;
} bkb_course_tag;

typedef struct bkb_course_chart
{
	bkb_str id;
	bkb_str bundle_id;
	bkb_str desc;
} bkb_course_chart;

typedef struct bkb_course
{
	bkb_str name;
	const bkb_course_chart* charts;
	size_t charts_len;
	const bkb_course_tag* tags;
	size_t tags_len;
	bkb_timestamp updated;
} bkb_course;

typedef struct bkb_collection_metadata
{
	bkb_str url;
} bkb_collection_metadata;

typedef struct bkb_collection_metadata_list
{
	const bkb_collection_metadata* items;
	size_t items_len;
} bkb_collection_metadata_list;

bkb_error_code bkb_store_open(bkb_store** out);
void bkb_store_free(bkb_store* store);
bkb_error_code bkb_store_get_bundle(bkb_store* store, const char* bundle_id, bkb_bb** out);
bkb_error_code bkb_store_resolve_path(bkb_store* store, const char* bundle_id, const char* relative_path, bkb_asset_data* out);
bkb_error_code bkb_store_sqlite_attach_command(bkb_store* store, bkb_string* out);
bkb_error_code bkb_store_list_tables(bkb_store* store, const char* const* gamemodes, size_t gamemodes_len, bkb_collection_metadata_list** out);
bkb_error_code bkb_store_get_table(bkb_store* store, const char* url, bkb_table** out);
bkb_error_code bkb_store_list_courses(bkb_store* store, const char* const* gamemodes, size_t gamemodes_len, bkb_collection_metadata_list** out);
bkb_error_code bkb_store_get_course(bkb_store* store, const char* url, bkb_course** out);
void bkb_table_free(bkb_table* table);
void bkb_course_free(bkb_course* course);
void bkb_collection_metadata_list_free(bkb_collection_metadata_list* list);
void bkb_bb_free(bkb_bb* bundle);
void bkb_asset_data_free(bkb_asset_data asset);
void bkb_string_free(bkb_string value);
const char* bkb_error_string(bkb_error_code code);

#ifdef __cplusplus
}
#endif
