#ifndef SE_ATLAS_H
#define SE_ATLAS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// A tile in an atlas. atlas_id is the sg_image id of the atlas itself
// x1, y1, x2, y2 are the coordinates of the tile in the atlas
typedef struct {
    uint32_t atlas_id;
    float x1, y1;
    float x2, y2;
} atlas_tile_t;

// There can be up to 256 atlas maps at a time, and each of them can have any number of atlases
// using different tile sizes
typedef uint8_t atlas_map_id_t;

// Each atlas tile id is not a pointer, but a descriptor that contains the map id
// and the necessary info to find a tile in said atlas map
typedef struct {
    uint16_t tile_width;
    uint16_t tile_height;
    atlas_map_id_t map_id;
    uint8_t tile_id[3]; // 24-bits of precision, more than enough to represent any number of tiles in our 
                        // supported tile sizes (width/height >= 8, atlas size <= 4096)
} atlas_tile_id_t;

atlas_map_id_t atlas_create_map();

void atlas_destroy_map(atlas_map_id_t map);

// Downloads an image from the url and adds it to the atlas map
atlas_tile_id_t atlas_add_tile_from_url(atlas_map_id_t map, const char* url);

// Loads an image from a path and adds it to the atlas map
atlas_tile_id_t atlas_add_tile_from_path(atlas_map_id_t map, const char* path, bool use_separate_thread);

// Checks if a tile is ready to be used
bool atlas_has_tile(atlas_tile_id_t id);

// Must be called after atlas_has_tile ensures the tile is ready
atlas_tile_t atlas_get_tile(atlas_tile_id_t id);

// Called from the main thread at the end of the frame to update the atlases if needed, for example if there's a need to resize
// or if there are new tiles to add
void atlas_upload_all();

// Useful to upload a single atlas map immediately. For example if you add a bunch of images from paths and want to
// make them immediately available
void atlas_upload_single(atlas_map_id_t map);

#ifdef __cplusplus
}
#endif

#endif