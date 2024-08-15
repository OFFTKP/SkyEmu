#include "atlas.h"
#include "sokol_gfx.h"

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

static_assert(sizeof(atlas_tile_id_t) == 8, "atlas_tile_id_t must be 8 bytes");
static_assert(sizeof(atlas_map_id_t) == 1, "atlas_map_id_t must be 1 byte"); 
static_assert(sizeof(atlas_tile_id_t::tile_id) == 3, "atlas_tile_id_t::tile_id must be 3 bytes");

[[noreturn]] void atlas_error(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);
    exit(1);
}

const int atlas_spacing = 4; // leaving some space between tiles to avoid bleeding

struct cached_image_t {
    uint8_t* data; // always RGBA
    int width;
    int height;
};

struct atlas_tile_info_t {
    cached_image_t* image_data;
    float x1, y1, x2, y2; // coordinates in the atlas
    std::atomic_bool ready = { false }; // whether the tile has been uploaded to the GPU
};

struct atlas_map_t;

std::mutex cached_images_mutex;
std::unordered_map<std::string, cached_image_t> cached_images;

std::mutex atlas_maps_mutex;
std::shared_ptr<atlas_map_t> atlas_maps[256] = {};

// Atlases are always square and power of two
// This always starts as a single tile image, but if a new tile needs to be
// added, it's resized to the next power of two
struct atlas_t {
    atlas_t(uint16_t tile_width, uint16_t tile_height)
        : tile_width(tile_width), tile_height(tile_height)
    {
        image.id = SG_INVALID_ID;
    }

    ~atlas_t() = default;
    atlas_t(const atlas_t&) = delete;
    atlas_t& operator=(const atlas_t&) = delete;
    atlas_t(atlas_t&&) = delete;
    atlas_t& operator=(atlas_t&&) = delete;

    std::mutex mutex;
    std::vector<uint8_t> data; // we construct the atlas here before uploading it to the GPU

    // Maps a tile id to its image data and its position in the atlas, which can change
    std::unordered_map<uint32_t, atlas_tile_info_t> tiles; 
    sg_image image = {};
    uint32_t pixel_stride = 0;
    uint16_t offset_x = 0, offset_y = 0;
    const uint16_t tile_width, tile_height;
    bool resized = false;
    bool dirty = false; // needs the data to be reuploaded to the GPU

    void add_image(cached_image_t* image)
    {
        std::unique_lock<std::mutex> lock(mutex);
        dirty = true;

        // First, check if we need to resize the atlas
        // TODO:


        uint16_t new_tile_offset_x = offset_x;
        uint16_t new_tile_offset_y = offset_y;

        offset_x += tile_width + atlas_spacing;
        if (offset_x + tile_width > pixel_stride)
        {
            offset_x = 0;
            offset_y += tile_width + atlas_spacing;
        }

        assert(image->width == tile_width);

        for (int y = 0; y < tile_height; y++)
        {
            for (int x = 0; x < tile_width; x++)
            {
                uint32_t atlas_index =
                    ((new_tile_offset_x + x) * 4) + (((new_tile_offset_y + y) * pixel_stride) * 4);
                uint32_t tile_index = x * 4 + (y * 4 * tile_width);

                assert(atlas_index + 3 < data.size());
                assert(tile_index + 3 < tile_width * tile_height * 4);

                data[atlas_index + 0] = image->data[tile_index + 0];
                data[atlas_index + 1] = image->data[tile_index + 1];
                data[atlas_index + 2] = image->data[tile_index + 2];
                data[atlas_index + 3] = image->data[tile_index + 3];
            }
        }
    }

private:
    void resize();
};

struct atlas_map_t {
    std::mutex atlases_mutex;
    std::unordered_map<uint32_t, std::shared_ptr<atlas_t>> atlases;
};

atlas_map_id_t atlas_create_map() {
    std::unique_lock<std::mutex> lock(atlas_maps_mutex);
    for (int i = 0; i < 256; i++) {
        if (atlas_maps[i] == nullptr) {
            atlas_maps[i] = std::make_shared<atlas_map_t>();
            return i;
        }
    }

    atlas_error("atlas_create_map: too many maps\n");
}

void atlas_destroy_map(atlas_map_id_t id) {
    if (atlas_maps[id] == nullptr) {
        atlas_error("atlas_destroy_map: map is null\n");
    }

    atlas_maps[id] = nullptr;
}

atlas_tile_id_t atlas_add_tile_from_url(atlas_map_t* map, const char* url) {

}

bool atlas_has_tile(atlas_tile_id_t id) {
    std::shared_ptr<atlas_map_t> map = atlas_maps[id.map_id];
    
    if (map == nullptr) {
        atlas_error("atlas_has_tile: map is null\n");
    }

    std::shared_ptr<atlas_t> atlas = nullptr;

    // Find atlas in atlas map
    {
        std::unique_lock<std::mutex> lock(map->atlases_mutex);

        auto it = map->atlases.find(id.tile_width << 16 | id.tile_height);
        if (it == map->atlases.end()) {
            atlas_error("atlas_has_tile: atlas not found\n");
        }

        atlas = it->second;
        if (atlas == nullptr) {
            atlas_error("atlas_has_tile: atlas is null\n");
        }
    }

    // Check for the tile in the atlas
    {
        std::unique_lock<std::mutex> lock(atlas->mutex);
        uint32_t tile_id = id.tile_id[0] | (id.tile_id[1] << 8) | (id.tile_id[2] << 16);
        auto it = atlas->tiles.find(tile_id);
        if (it == atlas->tiles.end()) {
            return false;
        }

        return it->second.ready;
    }
}

atlas_tile_t atlas_get_tile(atlas_tile_id_t id) {
    std::shared_ptr<atlas_map_t> map = atlas_maps[id.map_id];
    
    if (map == nullptr) {
        atlas_error("atlas_has_tile: map is null\n");
    }

    std::shared_ptr<atlas_t> atlas = nullptr;

    // Find atlas in atlas map
    {
        std::unique_lock<std::mutex> lock(map->atlases_mutex);

        auto it = map->atlases.find(id.tile_width << 16 | id.tile_height);
        if (it == map->atlases.end()) {
            atlas_error("atlas_has_tile: atlas not found\n");
        }

        atlas = it->second;
        if (atlas == nullptr) {
            atlas_error("atlas_has_tile: atlas is null\n");
        }
    }

    // Check for the tile in the atlas
    {
        std::unique_lock<std::mutex> lock(atlas->mutex);
        uint32_t tile_id = id.tile_id[0] | (id.tile_id[1] << 8) | (id.tile_id[2] << 16);
        auto it = atlas->tiles.find(tile_id);
        if (it == atlas->tiles.end()) {
            atlas_error("atlas_get_tile: tile not found\n");
        }

        atlas_tile_info_t& tile_info = it->second;
        atlas_tile_t tile = {};
        tile.atlas_id = atlas->image.id;
        tile.x1 = tile_info.x1;
        tile.y1 = tile_info.y1;
        tile.x2 = tile_info.x2;
        tile.y2 = tile_info.y2;

        return tile;
    }
}