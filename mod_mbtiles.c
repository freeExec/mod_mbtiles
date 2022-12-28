/*
	Vector tiles .mbtiles server module
	adapted from https://github.com/apeyroux/mod_osm by Jean-Alexandre Peyroux
	see also https://github.com/kd2org/apache-sqliteblob

	To install:
		sudo apxs -lsqlite3 -lzlib -i -a -c mod_mbtiles.c mbtiles_metadata.c && sudo service apache2 restart

	To configure Apache:
		MbtilesEnabled true
		MbtilesAdd vt "/path/to/my/vector_tiles.mbtiles"
		MbtilesAdd dem "/path/to/my/dem.mbtiles"
		MbtilesAddEx v2 vt "/path/to/my/vector_tiles_v2.mbtiles"
		MbtilesReturnEmptyTile Off

	Note that MbtilesEnabled applies per-directory, while MbtilesAdd is global (across all virtual hosts)
*/

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "http_request.h"
#include "util_script.h"
#include "http_connection.h"

#include "mod_core.h"

#include "apr_strings.h"

#include <sqlite3.h>
//#define SQLITE_API __declspec(dllimport)

#include <zlib.h>

#include <synchapi.h>

#define MOD_GZIP_ZLIB_WINDOWSIZE 15
#define MOD_GZIP_ZLIB_CFACTOR 9
#define MOD_GZIP_ZLIB_BSIZE 8096

#include "mbtiles_metadata.h"

#define ON 1
#define OFF 0

#define MATCH_NO		0
#define MATCH_OK		1
#define MATCH_COMPOSITE 2
#define MATCH_LONG_NAME 3

#define MAX_TILESETS 20
#define MAX_TILESET_NAME 40
#define MAX_FORMAT_NAME 8
#define MERGE_TILES_BUFFER_SIZE (4096 * 256)	// 1MB
#define METADATE_JSON_BUFFER_SIZE (4096 * 1)	// 1 page

typedef struct Tileset {
	int opened;
	char path[255];
	char version[MAX_TILESET_NAME];
	char name[MAX_TILESET_NAME];
	char format[MAX_FORMAT_NAME];
	int isPBF;
	sqlite3 *db;
} Tileset;

typedef struct DirectoryConfig {
	char context[256];
	int enabled;
	int return_empty_tile;
} DirectoryConfig;

typedef struct TileRequest {
	ap_regmatch_t name_position;
	int zoom;
	int x;
	int y;
	//char format[MAX_FORMAT_NAME];
	int metadata;
} TileRequest;

typedef struct TileRecord {
	unsigned char* compressedData;
	unsigned int   compressedSize;
	TilesetMetadata* metadata;
} TileRecord;

const char* const DEFAULT_VERSION = "-";

#define findTS(name) \
	findTileset(DEFAULT_VERSION, name)

static void mbtiles_register_hooks (apr_pool_t *p);
int findTileset(const char* version, const char* name);
int mbtiles_composite_handler(const request_rec* r);
void *mbtiles_create_dir_conf(apr_pool_t *pool, char *context);
void* mbtiles_merge_dir_conf(apr_pool_t* pool, void* BASE, void* ADD);
const char *mbtiles_add_path(cmd_parms *cmd, void *cfg, const char *name, const char *path);
const char *mbtiles_add_path_ext(cmd_parms *cmd, void *cfg, const char* version, const char *name, const char *path);
const char *mbtiles_set_enabled(cmd_parms *cmd, void *cfg, const char *arg);
const char* mbtiles_set_empty_tile(cmd_parms* cmd, void* cfg, const char* arg);
static int extractTileRequest(char* uri, TileRequest* tileRequest);
static apr_size_t decompressGzip(unsigned char* dest, apr_size_t buffer_size, unsigned char* source, apr_size_t size);
static apr_size_t compressGzip(unsigned char* dest, apr_size_t dsize, unsigned char* source, apr_size_t ssize, int compressionlevel);
bool mbtile_read_metadata(sqlite3* db, TilesetMetadata* metadata, apr_pool_t* pool);

static Tileset tilesets[MAX_TILESETS];
static int numLoaded = 0;
static apr_size_t dynamic_tiles_size = MERGE_TILES_BUFFER_SIZE;
//static DirectoryConfig config;

static ap_regex_t* regexpc_match_uri = NULL;
//const char* regexp_match_uri = "^\\/(?'path'[\\w\\/]+)\\/(?'z'\\d+)\\/(?'x'\\d+)\\/(?'y'\\d+)\\.(?'format'.*)$";

static unsigned char EMPTY_TILE[36] = { 0x1F,0x8B,0x08,0x00,0xFA,0x78,0x18,0x5E,0x00,0x03,0x93,0xE2,0xE3,0x62,0x8F,0x8F,0x4F,0xCD,0x2D,0x28,0xA9,
	0xD4,0x68,0x50,0xA8,0x60,0x02,0x00,0x64,0x71,0x44,0x36,0x10,0x00,0x00,0x00 };

static const command_rec mbtiles_directives[] = {
	AP_INIT_TAKE1("MbtilesEnabled", mbtiles_set_enabled, NULL, OR_ALL, "Enable or disable mod_mbtiles"),
	AP_INIT_TAKE2("MbtilesAdd", mbtiles_add_path, NULL, OR_ALL, "The tileset name and path to an .mbtiles file."),
	AP_INIT_TAKE3("MbtilesAddEx", mbtiles_add_path_ext, NULL, OR_ALL, "The tileset name and path to an .mbtiles file."),
	AP_INIT_TAKE1("MbtilesReturnEmptyTile", mbtiles_set_empty_tile, NULL, OR_ALL, "Return empty tile if tile not found."),
	{ NULL }
};

AP_DECLARE_MODULE(mbtiles);

module AP_MODULE_DECLARE_DATA mbtiles_module = {
	STANDARD20_MODULE_STUFF,
	mbtiles_create_dir_conf,/* Per-directory configuration handler */
	mbtiles_merge_dir_conf,	/* Merge handler for per-directory configurations */
	NULL,	/* Per-server configuration handler */
	NULL,	/* Merge handler for per-server configurations */
	mbtiles_directives,		/* Any directives we may have for httpd */
	mbtiles_register_hooks 	/* Our hook registering function */
};

void *mbtiles_create_dir_conf(apr_pool_t *pool, char *context) {
	context = context ? context : (char*)"(undefined context)";

	DirectoryConfig *cfg = (DirectoryConfig*)apr_pcalloc(pool, sizeof(DirectoryConfig));

	if (cfg) {
		strcpy(cfg->context, context);
		cfg->enabled = OFF;
		cfg->return_empty_tile = 0;
	}

	return cfg;
}

void* mbtiles_merge_dir_conf(apr_pool_t* pool, void* BASE, void* ADD) {
	DirectoryConfig* base = (DirectoryConfig*)BASE; /* This is what was set in the parent context */
	DirectoryConfig* add = (DirectoryConfig*)ADD;   /* This is what is set in the new context */
	DirectoryConfig* conf = (DirectoryConfig*)mbtiles_create_dir_conf(pool, "Merged configuration"); /* This will be the merged configuration */

	/* Merge configurations */
	conf->enabled = base->enabled || add->enabled;
	conf->return_empty_tile = base->return_empty_tile || add->return_empty_tile;
	return conf;
}

const char *mbtiles_set_enabled(cmd_parms *cmd, void *cfg, const char *arg) {
	DirectoryConfig *config = (DirectoryConfig*) cfg; // cast void pointer to DirectoryConfig
	if (!strcasecmp(arg, "true") || !strcasecmp(arg, "on"))
		config->enabled = ON;
	else
		config->enabled = OFF;

	return NULL;
}

const char *mbtiles_add_path(cmd_parms *cmd, void *cfg, const char *name, const char *path) {
	// we ignore config because tilesets are loaded globally
	if (findTS(name)>-1) return NULL; // don't reload if we already have one
	if (numLoaded==MAX_TILESETS) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, cmd->server, "Maximum tilesets already loaded");
		return NULL;
	}
	Tileset tileset;
	tileset.opened = OFF;
	strcpy(tileset.version, DEFAULT_VERSION);
	strcpy(tileset.path, path);
	strcpy(tileset.name, name);
	tilesets[numLoaded] = tileset;
	numLoaded++;

	return NULL;
}

const char *mbtiles_add_path_ext(cmd_parms *cmd, void *cfg, const char* version, const char *name, const char *path) {
	// we ignore config because tilesets are loaded globally
	if (findTileset(version, name)>-1) return NULL; // don't reload if we already have one
	if (numLoaded==MAX_TILESETS) {
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, cmd->server, "Maximum tilesets already loaded");
		return NULL;
	}
	Tileset tileset;
	tileset.opened = OFF;
	strcpy(tileset.version, version);
	strcpy(tileset.path, path);
	strcpy(tileset.name, name);
	tilesets[numLoaded] = tileset;
	numLoaded++;
	return NULL;
}

const char* mbtiles_set_empty_tile(cmd_parms* cmd, void* cfg, const char* arg) {
	DirectoryConfig* config = (DirectoryConfig*)cfg; // cast void pointer to DirectoryConfig
	if (!ap_cstr_casecmp(arg, "true") || !strcasecmp(arg, "on"))
		config->return_empty_tile = ON;
	else
		config->return_empty_tile = OFF;
	return NULL;
}

void processStarting(apr_pool_t *pool, server_rec *s) {
	//regexpc_match_uri = ap_pregcomp(pool, "^\\/?(?'path'[\\w\\/,-]+)\\/(?'z'\\d+)\\/(?'x'\\d+)\\/(?'y'\\d+)\\.(?'format'.*)$", (AP_REG_EXTENDED | AP_REG_ICASE));
	regexpc_match_uri = ap_pregcomp(pool, "^\\/?(?'v'[\\w]+\/)?\\/?(?'path'[\\w,-_]+)\\/(?'z'\\d+)\\/(?'x'\\d+)\\/(?'y'\\d+)\\.(?'format'.*)$", (AP_REG_EXTENDED | AP_REG_ICASE));
	ap_assert(regexpc_match_uri != NULL);

	for (int i=0; i<numLoaded; i++) {
		// Attempt to open the database
		if (SQLITE_OK!=sqlite3_open_v2(tilesets[i].path, &tilesets[i].db, SQLITE_OPEN_READONLY, NULL)) {
			sqlite3_close(tilesets[i].db);
			tilesets[i].opened = OFF;
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "Couldn't open mbtiles");
			return;
		}

		// Successfully opened, so find out what format it is
		const char *sql = "SELECT value FROM metadata WHERE name='format';";
		sqlite3_stmt *pStmt;
		int rc = sqlite3_prepare(tilesets[i].db, sql, -1, &pStmt, 0);
		if (rc!=SQLITE_OK) { ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "Couldn't find format in mbtiles"); return; }
		rc = sqlite3_step(pStmt);
		if (rc!=SQLITE_ROW) { ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "Couldn't find format in mbtiles"); return; }
		const char *fmt = sqlite3_column_text(pStmt, 0);
		strcpy_s(tilesets[i].format, MAX_FORMAT_NAME, fmt);
		rc = sqlite3_finalize(pStmt);

		// All good!
		tilesets[i].opened = ON;
		tilesets[i].isPBF = (strcmp(tilesets[i].format,"pbf")==0) ? 1 : 0;
		ap_log_error(APLOG_MARK, APLOG_INFO, 0, s, tilesets[i].isPBF ? "%s: successfully opened vector mbtiles" : "%s: successfully opened raster mbtiles", tilesets[i].name);
	}
}

static apr_status_t processEnding(void *d) {
	if (regexpc_match_uri)
		ap_regfree(regexpc_match_uri);
	for (int i=0; i<numLoaded; i++) {
		sqlite3_close(tilesets[i].db);
		tilesets[i].opened = OFF;
		//mbtiles_metadata_release(&tilesets[i].metadata);
	}
	return APR_SUCCESS;
}

static void mbtiles_register_hooks(apr_pool_t *p) {
	ap_hook_handler(mbtiles_composite_handler, NULL, NULL, APR_HOOK_FIRST);
	apr_pool_cleanup_register(p, NULL, processEnding, apr_pool_cleanup_null);
	ap_hook_child_init(processStarting, NULL, NULL, APR_HOOK_FIRST);
}

static int readTile(sqlite3 *db, const int z, const int x, const int y, apr_pool_t *pool, unsigned char **pTile, int *psTile ) {
	const char *sql = "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?;";
	sqlite3_stmt *pStmt;
	int rc; // sqlite return code
	*pTile = NULL;

	do {
		rc = sqlite3_prepare(db, sql, -1, &pStmt, 0);
		if(rc!=SQLITE_OK) { return rc; }

		sqlite3_bind_int(pStmt, 1, z);
		sqlite3_bind_int(pStmt, 2, x);
		sqlite3_bind_int(pStmt, 3, y);

		rc = sqlite3_step(pStmt);
		if( rc==SQLITE_ROW ){
			*psTile = sqlite3_column_bytes(pStmt, 0);
			*pTile = apr_palloc(pool, *psTile);
			if (*pTile == NULL)
				return SQLITE_NOMEM;
			memcpy(*pTile, sqlite3_column_blob(pStmt, 0), *psTile);
		}

		rc = sqlite3_finalize(pStmt);

	} while(rc==SQLITE_SCHEMA);

	return rc;
}

int findTileset(const char* version, const char* name) {
	for (int i=0; i<numLoaded; i++) {
		if (strcmp(tilesets[i].name, name)==0 && strcmp(tilesets[i].version, version)==0) { return i; }
	}
	return -1;
}

int mbtiles_composite_handler(const request_rec* r) {
	if (r->method_number == M_OPTIONS)
	{
		ap_allow_methods(r, 1, "GET", NULL);
		return ap_send_http_options(r);
	}

#ifndef TEST_MOD
	DirectoryConfig* config = (DirectoryConfig*)ap_get_module_config(r->per_dir_config, &mbtiles_module);
	if (config->enabled == OFF) return(DECLINED);
#endif

	TileRequest tileRequest;

	int isMatch = extractTileRequest(r->uri, &tileRequest);
	if (isMatch == MATCH_NO)
		return(DECLINED);	// pattern didn't match

	apr_size_t tile_name_last_position;
	apr_size_t tile_name_position;
	tile_name_last_position = tile_name_position = tileRequest.name_position.rm_so;
	TileRecord list_raw_tiles[MAX_TILESETS];

	char name[MAX_TILESET_NAME];

	unsigned int tile_count = 0;
	unsigned int tileSize = 0;
	unsigned char* tile = NULL;

	do	{
		char* separator = strchr(&r->uri[tile_name_position], ',');
		//apr_size_t offsetSeparator = index_of_char(&r->uri[tile_name_position], ',', tileRequest.name_position.rm_eo - tile_name_position);
		if (separator == NULL) {
			tile_name_position = tileRequest.name_position.rm_eo;
		}
		else {
			tile_name_position += separator - &r->uri[tile_name_position];
		}

		apr_size_t len = tile_name_position - tile_name_last_position;

		if (len == 0)
		{
			break;
		}

		errno_t copy_ret = strncpy_s(name, MAX_TILESET_NAME, &r->uri[tile_name_last_position], len);

		// find which tileset it is
		int c = findTS(name);
		if (c == -1) {
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "couldn't find tileset: %s", name);
			ap_set_content_type(r, "text/html");
			ap_rprintf(r, "couldn't find tileset: %s", name);
			return HTTP_NOT_FOUND;
		}
		if (tilesets[c].opened == 0) {
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "mbtiles file isn't open");
			return HTTP_INTERNAL_SERVER_ERROR;
		}

		if (tileRequest.metadata) {
			TilesetMetadata* metadata = apr_palloc(r->pool, sizeof(TilesetMetadata));
			TilesetMetadata metadata_default = tileset_metadata_init_default;
			memcpy(metadata, &metadata_default, sizeof(TilesetMetadata));

			mbtile_read_metadata(tilesets[c].db, metadata, r->pool);
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "readed mbtiles metadata OK");

			list_raw_tiles[tile_count].metadata = metadata;
			tile_count++;
		}
		// read tile
		else if (SQLITE_OK != readTile(tilesets[c].db, tileRequest.zoom, tileRequest.x, tileRequest.y, r->pool, &tile, &tileSize)) {
			// SQLite error
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "sqlite error while reading %s %d/%d/%d from mbtiles", name, tileRequest.zoom, tileRequest.x, tileRequest.y);
			sqlite3_close(tilesets[c].db);
			tilesets[c].opened = OFF;
			return HTTP_INTERNAL_SERVER_ERROR;
		} else if (NULL == tile && tilesets[c].isPBF) {
			// Vector tile not found
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "VTile %s %d/%d/%d not found", name, tileRequest.zoom, tileRequest.x, tileRequest.y);
		}
		else if (NULL == tile) {
			// Raster tile not found
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "RTile %s %d/%d/%d not found", name, tileRequest.zoom, tileRequest.x, tileRequest.y);
		}
		else if (tilesets[c].isPBF) {
#ifndef TEST_MOD
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Prepare vector tile (size:%d) : %s %d/%d/%d", tileSize, name, tileRequest.zoom, tileRequest.x, tileRequest.y);
#endif

			list_raw_tiles[tile_count].compressedData = tile;
			list_raw_tiles[tile_count].compressedSize = tileSize;
			//list_raw_tiles[tile_count].uncompressedData = NULL;
			//list_raw_tiles[tile_count].uncompressedSize = 0;
			tile_count++;
		}
		else {
			// Write raster tile
			ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Writing raster tile (size:%d) : %s %d/%d/%d", tileSize, name, tileRequest.zoom, tileRequest.x, tileRequest.y);
			if (strcmp(tilesets[c].format, "png") == 0) { ap_set_content_type(r, "image/png"); }
			else if (strcmp(tilesets[c].format, "jpg") == 0) { ap_set_content_type(r, "image/jpeg"); }
			else if (strcmp(tilesets[c].format, "webp") == 0) { ap_set_content_type(r, "image/webp"); }
			else { ap_set_content_type(r, tilesets[c].format); }
			ap_set_content_length(r, tileSize);
			ap_rwrite(tile, tileSize, r);
			return OK;
		}

		tile_name_position++;	// skip ,
		tile_name_last_position = tile_name_position;
	} while (tile_name_position < tileRequest.name_position.rm_eo);

	if (tile_count == 0)	{
		// tile not found
		//ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Tile %d/%d/%d not found", tileRequest.zoom, tileRequest.x, tileRequest.y);
#ifndef TEST_MOD
		if (config->return_empty_tile) {
			ap_set_content_type(r, "application/x-protobuf");
			apr_table_setn(r->headers_out, "Content-Encoding", "gzip");
			ap_set_content_length(r, 36);
			ap_rwrite(EMPTY_TILE, 36, r);
			return OK;
		}
		else
#endif
		{
			return HTTP_NOT_FOUND;
		}
	} else if (tile_count == 1)	{
		if (tileRequest.metadata) {
			mbtiles_metadata_fill_tiles(list_raw_tiles[0].metadata, r->hostname, NULL, name, r->pool);
			char* json = mbtiles_metadata_tojson(list_raw_tiles[0].metadata, r->pool);
			ap_set_content_type(r, "application/json");
			ap_rputs(json, r);
			return OK;
		}

		TileRecord* tileRecord = &list_raw_tiles[0];
		// Write vector tile
		ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, "Writing vector tile (size:%d) : %d/%d/%d", tileSize, tileRequest.zoom, tileRequest.x, tileRequest.y);
		ap_set_content_type(r, "application/x-protobuf");
		apr_table_setn(r->headers_out, "Content-Encoding", "gzip");
		ap_set_content_length(r, tileRecord->compressedSize);
		ap_rwrite(tileRecord->compressedData, tileRecord->compressedSize, r);

		return OK;
	}
	else {
		if (tileRequest.metadata) {
			TilesetMetadata* list_metadata = apr_palloc(r->pool, tile_count * sizeof(TilesetMetadata));
			for (int i = 0; i < tile_count; i++)
				list_metadata[i] = *list_raw_tiles[i].metadata;
			TilesetMetadata combined_metadata = mbtiles_metadata_merge(list_metadata, tile_count, r->pool);

			apr_size_t full_name_len = tileRequest.name_position.rm_eo - tileRequest.name_position.rm_so;
			char* full_name = apr_pstrmemdup(r->pool, &r->uri[tileRequest.name_position.rm_so], full_name_len);
			mbtiles_metadata_fill_tiles(&combined_metadata, r->hostname, NULL, full_name, r->pool);
			char* json = mbtiles_metadata_tojson(&combined_metadata, r->pool);
			ap_set_content_type(r, "application/json");
			ap_rputs(json, r);
			return OK;
		}

		unsigned char* raw_tiles_buffer = apr_palloc(r->pool, dynamic_tiles_size);
		if (!raw_tiles_buffer)
		{
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Out of memory (buffer %d bytes)", dynamic_tiles_size);
			return HTTP_INTERNAL_SERVER_ERROR;
		}
		apr_size_t usedBuffer = 0;

		for (unsigned int i = 0; i < tile_count; i++)
		{
			TileRecord* tileRecord = &list_raw_tiles[i];

			apr_size_t decompressedSize = decompressGzip(
				&raw_tiles_buffer[usedBuffer],
				dynamic_tiles_size - usedBuffer,
				tileRecord->compressedData,
				tileRecord->compressedSize
			);

			if (Z_BUF_ERROR == decompressedSize) {
				unsigned char* new_raw_tiles_buffer = apr_palloc(r->pool, dynamic_tiles_size + MERGE_TILES_BUFFER_SIZE);
				if (!new_raw_tiles_buffer) {
					ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Out of memory (buffer %d bytes)", dynamic_tiles_size);
					return HTTP_INTERNAL_SERVER_ERROR;
				}
				memcpy(new_raw_tiles_buffer, raw_tiles_buffer, dynamic_tiles_size);

				dynamic_tiles_size += MERGE_TILES_BUFFER_SIZE;
				raw_tiles_buffer = new_raw_tiles_buffer;

				i--;	// retry
				continue;
			}

			usedBuffer += decompressedSize;
		}

		// use all free space
		//TileRecord newTileRecord = { NULL, &raw_tiles_buffer[usedBuffer], 0, MERGE_TILES_BUFFER_SIZE - usedBuffer };
		//apr_size_t write_size = merge_tiles(&list_raw_tiles[0], tile_count, newTileRecord.uncompressedData, newTileRecord.uncompressedSize);
		//if (!write_size)
		//{
		//	ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed merging tiles");
		//	goto exit;
		//}
		//usedBuffer += write_size;
		//newTileRecord.uncompressedSize = write_size;

		//newTileRecord.compressedData = &raw_tiles_buffer[usedBuffer];
		//newTileRecord.compressedSize = MERGE_TILES_BUFFER_SIZE - usedBuffer;
		apr_size_t compressedSize = compressGzip(&raw_tiles_buffer[usedBuffer], dynamic_tiles_size - usedBuffer,
											     raw_tiles_buffer, usedBuffer, 6);

		if (!compressedSize)
		{
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "failed compressing tiles");
			return DONE;
		}
		//newTileRecord.compressedSize = compressedSize;

		ap_set_content_type(r, "application/x-protobuf");
		apr_table_setn(r->headers_out, "Content-Encoding", "gzip");
		ap_set_content_length(r, compressedSize);
		ap_rwrite(&raw_tiles_buffer[usedBuffer], compressedSize, r);
	}

	return OK;
}

#define REG_MATCH_INDEX_VERSION	1
#define REG_MATCH_INDEX_NAME	2
#define REG_MATCH_INDEX_ZOOM	3
#define REG_MATCH_INDEX_X		4
#define REG_MATCH_INDEX_Y		5
#define REG_MATCH_INDEX_FORMAT  6

static int extractTileRequest(char *uri, TileRequest *tileRequest)
{
	int len = strlen(uri);
	const char metadata_json[] = "/metadata.json";
	size_t meta_position = len - sizeof(metadata_json) + 1;
	if (!strcmpi(uri + meta_position, metadata_json)) {
		char* slash = strchr(&uri[1], '/');
		if (slash - uri == meta_position)
			tileRequest->name_position.rm_so = 1;
		else
			tileRequest->name_position.rm_so = slash - uri + 1;
		tileRequest->name_position.rm_eo = meta_position;
		//strcpy(tileRequest->format, &metadata_json[1]);
		tileRequest->metadata = ON;
		return MATCH_OK;
	}

	// pattern-matching: could replace this with a regex (http://svn.apache.org/repos/asf/httpd/sandbox/replacelimit/include/ap_regex.h)
	ap_regmatch_t regm[AP_MAX_REG_MATCH];
	if (AP_REG_NOMATCH == ap_regexec(regexpc_match_uri, uri, AP_MAX_REG_MATCH, regm, 0))
	{
		return MATCH_NO;	// pattern didn't match
	}

	tileRequest->metadata = OFF;

	ap_regmatch_t regm_i;

	regm_i = regm[REG_MATCH_INDEX_ZOOM];
	tileRequest->zoom = atoi(&uri[regm_i.rm_so]);
	regm_i = regm[REG_MATCH_INDEX_X];
	tileRequest->x = atoi(&uri[regm_i.rm_so]);
	regm_i = regm[REG_MATCH_INDEX_Y];
	tileRequest->y = atoi(&uri[regm_i.rm_so]);

	regm_i = regm[REG_MATCH_INDEX_FORMAT];
	len = regm_i.rm_eo - regm_i.rm_so;
	//strncpy_s(tileRequest->format, MAX_FORMAT_NAME, &uri[regm_i.rm_so], len);
	//tileRequest->format[len] = 0;

	// invert y for TMS
	tileRequest->y = ((1 << tileRequest->zoom) - tileRequest->y - 1);

	regm_i = regm[REG_MATCH_INDEX_NAME];
	len = regm_i.rm_eo - regm_i.rm_so;

	tileRequest->name_position = regm_i;
	//if (index_of_char(&uri[regm_i.rm_so], 0, ',', len) != -1)
	//{
	//	tileRequest->name[0] = 0;
	//	return MATCH_COMPOSITE;
	//}

	//if (len >= MAX_TILESET_NAME)
	//{
	//	return MATCH_LONG_NAME;
	//}

	//strncpy_s(tileRequest->name, MAX_TILESET_NAME, &uri[regm_i.rm_so], len);
	//tileRequest->name[len] = 0;

	return MATCH_OK;
}

static apr_size_t decompressGzip(unsigned char* dest, apr_size_t buffer_size, unsigned char* source, apr_size_t size) {
	z_stream zs;                        // z_stream is zlib's control structure

	memset(&zs, 0, sizeof(zs));

	if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
		return 0;

	zs.next_in = (Bytef*)source;
	zs.avail_in = (uInt)size;

	int ret;
	zs.next_out = (Bytef*)(dest);
	zs.avail_out = (uInt)buffer_size;

	ret = inflate(&zs, Z_FINISH);

	if (Z_BUF_ERROR == ret)
		return Z_BUF_ERROR;

	return zs.total_out;
}

static apr_size_t compressGzip(unsigned char* dest, apr_size_t dsize,
							   unsigned char* source, apr_size_t ssize,
							   int compressionlevel) {
	z_stream zs;                        // z_stream is zlib's control structure
	memset(&zs, 0, sizeof(zs));

	if (deflateInit2(&zs, compressionlevel, Z_DEFLATED,
		MOD_GZIP_ZLIB_WINDOWSIZE + 16, MOD_GZIP_ZLIB_CFACTOR, Z_DEFAULT_STRATEGY) != Z_OK)
		return 0;	// deflateInit2 failed while compressing.

	zs.next_in = source;
	zs.avail_in = ssize;           // set the z_stream's input

	int ret;

	zs.next_out = (Bytef*)(dest);
	zs.avail_out = dsize;

	ret = deflate(&zs, Z_FINISH);

	deflateEnd(&zs);

	if (ret != Z_STREAM_END)
		return 0;	// Exception during zlib compression: (" << ret << ") " << zs.msg

	return zs.total_out;
}

bool mbtile_read_metadata(sqlite3* db, TilesetMetadata* metadata, apr_pool_t* pool) {
	const char* sql = "SELECT * FROM metadata;";
	sqlite3_stmt* pStmt;

	int rc;
	rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, NULL);
	if (rc != SQLITE_OK) { return false; }

	do {
		rc = sqlite3_step(pStmt);
		if (rc == SQLITE_ROW) {
			char* name = sqlite3_column_text(pStmt, 0);
			char* value = sqlite3_column_text(pStmt, 1);

			mbtiles_metadata_parse(name, value, metadata, pool);
		}
	} while (rc == SQLITE_ROW);

	rc = sqlite3_finalize(pStmt);

	return true;
}