#ifndef SE_ATLAS_H
#define SE_ATLAS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// A tile in an atlas. atlas_id is the sg_image id of the atlas itself
// x1, y1, x2, y2 are the coordinates of the tile in the atlas
typedef struct {
    uint32_t atlas_id;
    float x1, y1;
    float x2, y2;
} atlas_tile_t;

typedef uint32_t atlas_tile_id;

// An atlas map is a map of atlases, which themselves are images with multiple tiles
typedef struct atlas_map_t atlas_map_t;

atlas_map_t* atlas_create_map();

void atlas_destroy_map(atlas_map_t* map);

// Downloads an image from the url and adds it to the atlas map
atlas_tile_id atlas_add_tile_from_url(atlas_map_t* map, const char* url);

// Loads an image from a path and adds it to the atlas map
atlas_tile_id atlas_add_tile_from_path(atlas_map_t* map, const char* path, bool use_separate_thread);

// Checks if a tile is ready to be used
bool atlas_has_tile(atlas_map_t* map, atlas_tile_id id);

// Must be called after atlas_has_tile ensures the tile is ready
atlas_tile_t atlas_get_tile(atlas_map_t* map, atlas_tile_id id);

// Called from the main thread at the end of the frame to update the atlases if needed, for example if there's a need to resize
// or if there are new tiles to add
void atlas_update_all();

void atlas_update_immediately(atlas_map_t* map);

void atlas_update_all_immediately();

#ifdef __cplusplus
}
#endif

#endif