#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "apr_pools.h"
#include "apr_strings.h"

#include "mbtiles_metadata.h"

static const char* MetadateRequiredField[] = {
	"name",
	"format",
	"minzoom",
	"maxzoom",
	"bounds",
	"type",
	"tiles",
	"scheme",
	"attribution",
	"version",
	"json",


	"vector_layers"	// part of json
};

typedef enum MetadateIndex {
	MDI_NAME,
	MDI_FORMAT,
	MDI_MIN_ZOOM,
	MDI_MAX_ZOOM,
	MDI_BOUNDS,
	MDI_TYPE,
	MDI_TILES,
	MDI_SCHEME,
	MDI_ATTRIBUTION,
	MDI_VERSION,
	MDI_JSON,

	MDI_VECTOR_LAYERS,	// only output

	MDI_INPUT_COUNT = MDI_JSON + 1,
} MetadateIndex;

const char* vector_layers_pattern = "\"vector_layers\":[";

int write_json_begin(char* json, unsigned int buffer_len);
int write_json_prop(char* json, unsigned int buffer_len, char* name, char* value);
int write_json_object(char* json, unsigned int buffer_len, char* name, char* value);
int write_json_end(char* json, unsigned int buffer_len);
int write_json_next(char* json, unsigned int buffer_len);
int write_json_value(char* json, unsigned int buffer_len, char* value);

int find_pos(char* text, char* pattern);

void mbtiles_metadata_release(TilesetMetadata* metadata) {
	free(metadata->name);
	metadata->name = NULL;

	free(metadata->attribution);
	metadata->attribution = NULL;

	free(metadata->tiles);
	metadata->tiles = NULL;

	free(metadata->vector_layers);
	metadata->vector_layers = NULL;
}

void mbtiles_metadata_fill_tiles(TilesetMetadata* metadata, char* server_name, char* version, char* full_name, apr_pool_t* pool) {
	//metadata->tiles = "https://" + server_name + "/" + version + "/" + full_name + "/{z}/{x}/{y}." + format;
	const char* http_str = "https://";
	const char* zxy_str = "/{z}/{x}/{y}";

	apr_size_t len = strlen(http_str) + strlen(server_name) + 1;
	if (version)
		len += strlen(version) + 1;
	len += strlen(full_name) + strlen(zxy_str);
	if (metadata->format)
		len += 1 + strlen(metadata->format);
	len++;

	metadata->tiles = apr_palloc(pool, len);
	*metadata->tiles = 0;
	strcat_s(metadata->tiles, len, http_str);
	strcat_s(metadata->tiles, len, server_name);
	strcat_s(metadata->tiles, len, "/");
	if (version) {
		strcat_s(metadata->tiles, len, server_name);
		strcat_s(metadata->tiles, len, "/");
	}
	strcat_s(metadata->tiles, len, full_name);
	strcat_s(metadata->tiles, len, zxy_str);
	if (metadata->format) {
		strcat_s(metadata->tiles, len, ".");
		strcat_s(metadata->tiles, len, metadata->format);
	}

	if (metadata->tiles[0] == '/') {
		int gg = 99;
	}
}

TilesetMetadata mbtiles_metadata_merge(TilesetMetadata* metadata, int tilesets_count, apr_pool_t *pool) {
	TilesetMetadata combine_md = tileset_metadata_init_default;

	const char name_separator[] = " + ";
	const char attribution_seporator[] = " | ";
	const char vector_layers_seporator[] = ",";

	size_t combine_name_length = 0;
	size_t combine_attribution_length = 0;
	size_t combine_vector_layers_length = 0;

	bool* unique_attribution = apr_palloc(pool, tilesets_count * sizeof(bool));

	for (int t = 0; t < tilesets_count; t++) {
		TilesetMetadata* md = &metadata[t];

		if (md->name)
			combine_name_length += strlen(md->name) + sizeof(name_separator);
		if (md->attribution)
			combine_attribution_length += strlen(md->attribution) + sizeof(attribution_seporator);
		if (md->vector_layers)
			combine_vector_layers_length += strlen(md->vector_layers) + sizeof(vector_layers_seporator);

		if (!md->attribution)
			unique_attribution[t] = false;
		else {
			unique_attribution[t] = true;
			for (int a = 0; a < t; a++) {
				if (!unique_attribution[a]) continue;

				TilesetMetadata* mda = &metadata[a];
				int is_match = strcmpi(mda->attribution, md->attribution);
				if (is_match == 0) {
					unique_attribution[t] = false;
					break;
				}
			}
		}

	}
	if (combine_name_length > 0)
		combine_name_length -= sizeof(name_separator);
	if (combine_attribution_length > 0)
		combine_attribution_length -= sizeof(attribution_seporator);
	if (combine_vector_layers_length > 0)
		combine_vector_layers_length -= sizeof(vector_layers_seporator);

	combine_name_length++;
	combine_md.name = apr_palloc(pool, combine_name_length);
	*combine_md.name = 0;

	combine_attribution_length++;
	combine_md.attribution = apr_palloc(pool, combine_attribution_length);
	*combine_md.attribution = 0;

	combine_vector_layers_length++;
	combine_md.vector_layers = apr_palloc(pool, combine_vector_layers_length);
	*combine_md.vector_layers = 0;

	combine_md.format = apr_pstrdup(pool, metadata[0].format);
	combine_md.version = 1;

	for (int t = 0; t < tilesets_count; t++) {
		TilesetMetadata* md = &metadata[t];

		if (t > 0)
			strcat_s(combine_md.name, combine_name_length, name_separator);
		if (md->name)
			strcat_s(combine_md.name, combine_name_length, md->name);

		if (unique_attribution[t]) {
			if (combine_md.attribution[0])
				strcat_s(combine_md.attribution, combine_attribution_length, attribution_seporator);
			if (md->attribution)
				strcat_s(combine_md.attribution, combine_attribution_length, md->attribution);
		}

		if (combine_md.vector_layers[0])
			strcat_s(combine_md.vector_layers, combine_vector_layers_length, vector_layers_seporator);
		if (md->vector_layers)
			strcat_s(combine_md.vector_layers, combine_vector_layers_length, md->vector_layers);

		if (md->min_zoom != NOT_SET_ZOOM && (combine_md.min_zoom == NOT_SET_ZOOM || combine_md.min_zoom > md->min_zoom))
			combine_md.min_zoom = md->min_zoom;
		if (md->max_zoom != NOT_SET_ZOOM && (combine_md.max_zoom == NOT_SET_ZOOM || combine_md.max_zoom < md->max_zoom))
			combine_md.max_zoom = md->max_zoom;

		for (int b = 0; b < 4; b++) {
			if (md->bounds[b] != NOT_SET_BOUNDS &&
				(combine_md.bounds[b] == NOT_SET_BOUNDS ||
					b < 2 && combine_md.bounds[b] > md->bounds[b] ||
					b >= 2 && combine_md.bounds[b] < md->bounds[b]))
				combine_md.bounds[b] = md->bounds[b];
		}
	}

	return combine_md;
}

bool mbtiles_metadata_parse(char* name, char* value, TilesetMetadata* metadata, apr_pool_t* pool) {

	bool md_predefine_found = false;

	for (int field_index = 0; field_index < MDI_INPUT_COUNT; field_index++)
	{
		if (strcmp(MetadateRequiredField[field_index], name) == 0)
		{
			if (MDI_NAME == field_index) {
				size_t len = strlen(value) + 1;
				metadata->name = apr_pstrdup(pool, value);
			}
			else if (MDI_FORMAT == field_index) {
				metadata->format = apr_pstrdup(pool, value);
			}
			else if (MDI_ATTRIBUTION == field_index) {
				size_t len = strlen(value) + 1;
				metadata->attribution = apr_pstrdup(pool, value);
			}
			else if (MDI_TILES == field_index) {
				size_t len = strlen(value) + 1;
				metadata->tiles = apr_pstrdup(pool, value);
			}
			else if (MDI_MIN_ZOOM == field_index) {
				metadata->min_zoom = atoi(value);
			}
			else if (MDI_MAX_ZOOM == field_index) {
				metadata->max_zoom = atoi(value);
			} else if (MDI_VERSION == field_index) {
				metadata->version = atof(value);
			} else if (MDI_BOUNDS == field_index) {
				char* next_bounds = value;
				for (unsigned int i = 0; i < 4; i++)
				{
					if (i > 0) next_bounds = strchr(next_bounds, ',') + 1;
					metadata->bounds[i] = atof(next_bounds);
				}
			} else if (MDI_JSON == field_index) {
				int pos = find_pos(value, vector_layers_pattern);
				if (pos != -1) {
					int open_array = find_pos(&value[pos], "[");
					int close_array = find_pos(&value[pos], "]");
					if (open_array != -1 && close_array != -1) {
						open_array++;
						int len = close_array - open_array;
						metadata->vector_layers = apr_pstrmemdup(pool, &value[pos + open_array], len);
					}
				}
			}

			md_predefine_found = true;
			break;
		}
	}

	const apr_size_t CUSTON_LEN_ADDED = 4096;

	if (!md_predefine_found) {
		if (metadata->custom_len == 0) {
			metadata->custom_len = CUSTON_LEN_ADDED;
			metadata->custom_json = apr_palloc(pool, metadata->custom_len);
			metadata->custom_json[0] = 0;
		}

		apr_size_t len = 1 + 2 + 1 + 2 + strlen(name) + strlen(value);
		if (strlen(metadata->custom_json) + len >= metadata->custom_len) {
			char* new_custom_json = apr_palloc(pool, metadata->custom_len + (len < CUSTON_LEN_ADDED ? CUSTON_LEN_ADDED : len));
			strcpy(new_custom_json, metadata->custom_json);
			metadata->custom_json = new_custom_json;
		}

		if (metadata->custom_json[0]) {
			strcat_s(metadata->custom_json, metadata->custom_len, ",");
		}
		strcat_s(metadata->custom_json, metadata->custom_len, "\"");
		strcat_s(metadata->custom_json, metadata->custom_len, name);
		strcat_s(metadata->custom_json, metadata->custom_len, "\":\"");
		strcat_s(metadata->custom_json, metadata->custom_len, value);
		strcat_s(metadata->custom_json, metadata->custom_len, "\"");
	}

	return md_predefine_found;
}

int find_pos(const char* string, const char* substring) {
	const char* find = strstr(string, substring);
	if (find)
		return find - string;
	return -1;
}

static size_t mbtiles_metadata_tojson_length(TilesetMetadata* metadata) {
	size_t len = 0;
	if (metadata->name)
		len += strlen(metadata->name);
	if (metadata->attribution)
		len += strlen(metadata->attribution);
	if (metadata->format)
		len += strlen(metadata->format);
	if (metadata->vector_layers)
		len += strlen(metadata->vector_layers);
	len += (3 + 1 + 6) * 4;		// bounds
	len += 2 * 2;				// zoom
	len += 6;					// version
	len += 6 * MDI_INPUT_COUNT;

	return len;
}

char* mbtiles_metadata_tojson(TilesetMetadata* metadata, apr_pool_t* pool) {
	size_t len = mbtiles_metadata_tojson_length(metadata) * 2;
	char* json = apr_palloc(pool, len);
	if (!json)
		return NULL;
	char* json_debug = json;

	unsigned int json_text_index = 0;

#define JSON_POINT (&json[json_text_index])
#define JSON_BIFFER_LEN (len - json_text_index)

	json_text_index += write_json_begin(JSON_POINT, JSON_BIFFER_LEN);
	bool json_empty = true;

	char* name;
	char* value;

	// name
	value = metadata->name;
	if (value) {
		name = MetadateRequiredField[MDI_NAME];
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		json_text_index += write_json_prop(JSON_POINT, JSON_BIFFER_LEN, name, value);
		json_empty = false;
	}

	value = metadata->format;
	if (value) {
		name = MetadateRequiredField[MDI_FORMAT];
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		json_text_index += write_json_prop(JSON_POINT, JSON_BIFFER_LEN, name, value);
		json_empty = false;
	}

	value = metadata->attribution;
	if (value) {
		name = MetadateRequiredField[MDI_ATTRIBUTION];
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		json_text_index += write_json_prop(JSON_POINT, JSON_BIFFER_LEN, name, value);
		json_empty = false;
	}

	value = metadata->tiles;
	if (value) {
		name = MetadateRequiredField[MDI_TILES];
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		json_text_index += write_json_object(JSON_POINT, JSON_BIFFER_LEN, name, "[");
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, "\"");
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, value);
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, "\"");
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, "]");
		json_empty = false;
	}

	char int2string_buffer[25];

	if (metadata->min_zoom != NOT_SET_ZOOM)	{
		_itoa_s(metadata->min_zoom, int2string_buffer, sizeof(int2string_buffer), 10);
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		name = MetadateRequiredField[MDI_MIN_ZOOM];
		json_text_index += write_json_object(JSON_POINT, JSON_BIFFER_LEN, name, int2string_buffer);
		json_empty = false;
	}

	if (metadata->max_zoom != NOT_SET_ZOOM) {
		_itoa_s(metadata->max_zoom, int2string_buffer, sizeof(int2string_buffer), 10);
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		name = MetadateRequiredField[MDI_MAX_ZOOM];
		json_text_index += write_json_object(JSON_POINT, JSON_BIFFER_LEN, name, int2string_buffer);
		json_empty = false;
	}

	if (metadata->version != NOT_SET_VERSION) {
		if (metadata->version == (int)metadata->version) {
			_itoa_s(metadata->version, int2string_buffer, sizeof(int2string_buffer), 3);
		}
		else {
			_gcvt_s(int2string_buffer, sizeof(int2string_buffer), metadata->version, 3 + 2);
		}
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		name = MetadateRequiredField[MDI_VERSION];
		json_text_index += write_json_object(JSON_POINT, JSON_BIFFER_LEN, name, int2string_buffer);
		json_empty = false;
	}

	if (metadata->bounds[0] != NOT_SET_BOUNDS) {
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		name = MetadateRequiredField[MDI_BOUNDS];
		json_text_index += write_json_object(JSON_POINT, JSON_BIFFER_LEN, name, "[");

		for (int i = 0; i < 4; i++) {
			_gcvt_s(int2string_buffer, sizeof(int2string_buffer), metadata->bounds[i], 3+6);
			int len = strlen(int2string_buffer);
			if (int2string_buffer[len - 1] == '.') {
				int2string_buffer[len - 1] = 0;
			}
			if (i > 0) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
			json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, int2string_buffer);
		}

		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, "]");
		json_empty = false;
	}

	value = metadata->vector_layers;
	if (value) {
		name = MetadateRequiredField[MDI_VECTOR_LAYERS];
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		json_text_index += write_json_object(JSON_POINT, JSON_BIFFER_LEN, name, "[");
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, value);
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, "]");
		json_empty = false;
	}

	value = metadata->custom_json;
	if (value) {
		if (!json_empty) json_text_index += write_json_next(JSON_POINT, JSON_BIFFER_LEN);
		json_text_index += write_json_value(JSON_POINT, JSON_BIFFER_LEN, value);
		json_empty = false;
	}

	json_text_index += write_json_end(JSON_POINT, JSON_BIFFER_LEN);
	JSON_POINT[0] = '\0';

	return json;
}

int write_json_begin(char* json, unsigned int buffer_len) {
	const unsigned int size = 1;
	if (buffer_len >= size)
		json[0] = '{';
	return size;
}

int write_json_end(char* json, unsigned int buffer_len) {
	const unsigned int size = 1;
	if (buffer_len >= size)
		json[0] = '}';
	return size;
}

int write_json_next(char* json, unsigned int buffer_len) {
	const unsigned int size = 1;
	if (buffer_len >= size)
		json[0] = ',';
	return size;
}

int write_json_prop(char* json, unsigned int buffer_len, char* name, char* value) {
	unsigned int name_length = strlen(name);
	unsigned int value_length = strlen(value);

	if (buffer_len < 1 + name_length + 3 + value_length + 1)
		return 0;

	char* json_start = json;

	*json = '"'; json++;
	memcpy(json, name, name_length); json += name_length;
	*json = '"';	json++;
	*json = ':';	json++;
	*json = '"';	json++;
	memcpy(json, value, value_length); json += value_length;
	*json = '"';	json++;

	*json = 0;

	return json - json_start;
}

int write_json_object(char* json, unsigned int buffer_len, char* name, char* value) {

	unsigned int name_length = strlen(name);
	unsigned int value_length = strlen(value);
	if (buffer_len < 1 + name_length + 2 + value_length)
		return 0;

	char* json_start = json;

	*json = '"'; json++;
	memcpy(json, name, name_length); json += name_length;
	*json = '"';	json++;
	*json = ':';	json++;
	memcpy(json, value, value_length); json += value_length;

	*json = 0;

	return json - json_start;
}

int write_json_value(char* json, unsigned int buffer_len, char* value) {
	unsigned int value_length = strlen(value);

	if (buffer_len < value_length)
		return 0;

	char* json_start = json;

	memcpy(json, value, value_length); json += value_length;

	*json = 0;

	return json - json_start;
}