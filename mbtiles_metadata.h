#pragma once
#ifndef MBTILES_METADADA_H
#define MBTILES_METADADA_H

#include <stdbool.h>

typedef struct TilesetMetadate {
	/* MUST */
	char* name;
	char* format;
	/* SHOULD */
	float bounds[4];
	int center[3];
	int min_zoom;
	int max_zoom;
	/* MAY */
	float version;	// wtf - mapboxgl still looking
	char* attribution;
	char* tiles;
	char* vector_layers;
	char* custom_json;
	apr_size_t custom_len;
} TilesetMetadata;

#define NOT_SET_ZOOM -1
#define NOT_SET_VERSION -1
#define NOT_SET_BOUNDS -200
#define NOT_SET_CENTER -200

#define tileset_metadata_init_default { \
	NULL, \
	NULL, \
	NOT_SET_BOUNDS, NOT_SET_BOUNDS, NOT_SET_BOUNDS, NOT_SET_BOUNDS, \
	NOT_SET_CENTER, NOT_SET_CENTER, NOT_SET_ZOOM, \
	NOT_SET_ZOOM, NOT_SET_ZOOM, \
	NOT_SET_VERSION, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	0, \
}

//void mbtiles_metadata_release(TilesetMetadata* metadata);
bool mbtiles_metadata_parse(char* name, char* value, TilesetMetadata* metadata, apr_pool_t* pool);
TilesetMetadata mbtiles_metadata_merge(TilesetMetadata* metadata, int tilesets_count, apr_pool_t* pool);
static size_t mbtiles_metadata_tojson_length(TilesetMetadata* metadata);
char* mbtiles_metadata_tojson(TilesetMetadata* metadata, apr_pool_t* pool);
void mbtiles_metadata_fill_tiles(TilesetMetadata* metadata, char* server_name, char* version, char* full_name, apr_pool_t* pool);

#endif	// MBTILES_METADADA_H
