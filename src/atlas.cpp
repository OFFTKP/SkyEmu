#include "atlas.h"
#include "sokol_gfx.h"

#include <cassert>
#include <unordered_map>
#include <vector>

const int atlas_spacing = 4; // leaving some space between tiles to avoid bleeding

struct tile_size_t {
    uint16_t x, y;
};

struct cached_image_t {
    uint8_t* data; // always RGBA
    int width;
    int height;
};

// Atlases are always square and power of two
// This always starts as a single tile image, but if a new tile needs to be
// added, it's resized to the next power of two
struct atlas_t {
    atlas_t(uint32_t tile_width, uint32_t tile_height)
        : tile_width(tile_width), tile_height(tile_height)
    {
        image.id = SG_INVALID_ID;
    }

    ~atlas_t() = default;
    atlas_t(const atlas_t&) = delete;
    atlas_t& operator=(const atlas_t&) = delete;
    atlas_t(atlas_t&&) = default;
    atlas_t& operator=(atlas_t&&) = default;

    std::vector<uint8_t> data; // we construct the atlas here before uploading it to the GPU
    sg_image image = {};
    int pixel_stride = 0;
    int offset_x = 0,
        offset_y = 0; // to keep track of where next tile needs to be placed, in pixels
    int tile_width, tile_height;
    bool resized = false;
    bool dirty = false; // needs the data to be reuploaded to the GPU

    void add_image(cached_image_t* image)
    {
        dirty = true;

        uint32_t tile_offset_x = offset_x;
        uint32_t tile_offset_y = offset_y;

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
                uint32_t atlas_offset =
                    ((tile_offset_x + x) * 4) + (((tile_offset_y + y) * pixel_stride) * 4);
                uint32_t tile_offset = x * 4 + (y * 4 * tile_width);

                assert(atlas_offset + 3 < data.size());
                assert(tile_offset + 3 < tile_width * tile_height * 4);

                data[atlas_offset + 0] = image->data[tile_offset + 0];
                data[atlas_offset + 1] = image->data[tile_offset + 1];
                data[atlas_offset + 2] = image->data[tile_offset + 2];
                data[atlas_offset + 3] = image->data[tile_offset + 3];
            }
        }
    }
};

struct atlas_map_t {
    std::unordered_map<tile_size_t, atlas_t> atlases;
};