#include "backbeat.h"

bkb_error_code bkb_store_open(bkb_store** out)
{
	if (out)
		*out = NULL;
	return BKB_ERR_UNSUPPORTED;
}

void bkb_store_free(bkb_store* store)
{
	(void)store;
}

bkb_error_code bkb_store_get_bundle(bkb_store* store, const char* bundle_id, bkb_bb** out)
{
	(void)store;
	(void)bundle_id;
	if (out)
		*out = NULL;
	return BKB_ERR_UNSUPPORTED;
}

bkb_error_code bkb_store_resolve_path(bkb_store* store, const char* bundle_id, const char* relative_path, bkb_asset_data* out)
{
	(void)store;
	(void)bundle_id;
	(void)relative_path;
	if (out)
		out->kind = BKB_ASSET_DATA_NONE;
	return BKB_ERR_UNSUPPORTED;
}

bkb_error_code bkb_store_sqlite_attach_command(bkb_store* store, bkb_string* out)
{
	(void)store;
	if (out)
		out->ptr = NULL, out->len = 0;
	return BKB_ERR_UNSUPPORTED;
}

bkb_error_code bkb_store_list_tables(bkb_store* store, const char* const* gamemodes, size_t gamemodes_len, bkb_collection_metadata_list** out)
{
	(void)store;
	(void)gamemodes;
	(void)gamemodes_len;
	if (out)
		*out = NULL;
	return BKB_ERR_UNSUPPORTED;
}

bkb_error_code bkb_store_get_table(bkb_store* store, const char* url, bkb_table** out)
{
	(void)store;
	(void)url;
	if (out)
		*out = NULL;
	return BKB_ERR_UNSUPPORTED;
}

bkb_error_code bkb_store_list_courses(bkb_store* store, const char* const* gamemodes, size_t gamemodes_len, bkb_collection_metadata_list** out)
{
	(void)store;
	(void)gamemodes;
	(void)gamemodes_len;
	if (out)
		*out = NULL;
	return BKB_ERR_UNSUPPORTED;
}

bkb_error_code bkb_store_get_course(bkb_store* store, const char* url, bkb_course** out)
{
	(void)store;
	(void)url;
	if (out)
		*out = NULL;
	return BKB_ERR_UNSUPPORTED;
}

void bkb_table_free(bkb_table* table)
{
	(void)table;
}

void bkb_course_free(bkb_course* course)
{
	(void)course;
}

void bkb_collection_metadata_list_free(bkb_collection_metadata_list* list)
{
	(void)list;
}

void bkb_bb_free(bkb_bb* bundle)
{
	(void)bundle;
}

void bkb_asset_data_free(bkb_asset_data asset)
{
	(void)asset;
}

void bkb_string_free(bkb_string value)
{
	(void)value;
}

const char* bkb_error_string(bkb_error_code code)
{
	switch (code)
	{
	case BKB_OK: return "ok";
	case BKB_ERR_UNSUPPORTED: return "backbeat SDK is not available on this platform";
	default: return "unknown backbeat error";
	}
}
