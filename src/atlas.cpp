#include "sokol_gfx.h"
#include "atlas.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

[[noreturn]] void atlas_error(const char* message) {
    fprintf(stderr, "Atlas error: %s\n", message);
    exit(1);
}

struct cached_image_t {
    const uint8_t* data;
    uint32_t width, height;
};

std::mutex image_cache_mutex;
std::unordered_map<std::string, cached_image_t> image_cache;

struct atlas_t {
    atlas_t(uint32_t tile_width, uint32_t tile_height);
    ~atlas_t();

    atlas_tile_t* add_image(cached_image_t* image);
    void upload();

private:
    void copy_to_data(cached_image_t* image);

    const uint32_t tile_width, tile_height;

    std::mutex mutex;
    sg_image image;
    uint32_t atlas_dimension;
    uint32_t offset_x, offset_y;
    std::vector<uint8_t> data;
    std::unordered_map<cached_image_t*, atlas_tile_t*> images;
    std::vector<cached_image_t*> images_to_add;
    bool dirty;   // new data needs to be uploaded to the GPU
    bool resized; // atlas needs to be destroyed and created at new size

    constexpr static uint32_t padding = 4;
};

struct atlas_map_t {
    atlas_tile_t* add_image_from_url(const char* url);
    atlas_tile_t* add_image_from_path(const char* path);

private:
    std::mutex atlases_mutex;
    std::vector<atlas_t*> atlases;
};

atlas_t::atlas_t(uint32_t tile_width, uint32_t tile_height) : tile_width(tile_width), tile_height(tile_height) {
    image.id = SG_INVALID_ID;
    offset_x = 0;
    offset_y = 0;
    dirty = false;
    resized = false;
    atlas_dimension = 16;
    uint32_t minimum_width = tile_width + padding;
    uint32_t minimum_height = tile_height + padding;
    while (atlas_dimension < minimum_width || atlas_dimension < minimum_height) {
        atlas_dimension *= 2;
    }

    data.resize(atlas_dimension * atlas_dimension * 4);
}

void atlas_t::copy_to_data(cached_image_t* cached_image) {
    if (cached_image->width != tile_width || cached_image->height != tile_height) {
        atlas_error("Image dimensions do not match atlas tile dimensions");
    }

    dirty = true;

    uint32_t tile_offset_x = offset_x;
    uint32_t tile_offset_y = offset_y;

    offset_x += tile_width + padding;
    if (offset_x + tile_width > atlas_dimension) {
        offset_x = 0;
        offset_y += tile_height + padding;

        if (offset_y + tile_height > atlas_dimension) {
            atlas_error("Atlas is full somehow");
        }
    }

    for (int y = 0; y < tile_height; y++) {
        for (int x = 0; x < tile_width; x++) {
            uint32_t atlas_offset = ((tile_offset_x + x) * 4) + (((tile_offset_y + y) * atlas_dimension) * 4);
            uint32_t tile_offset = x * 4 + (y * 4 * tile_width);

            data[atlas_offset + 0] = cached_image->data[tile_offset + 0];
            data[atlas_offset + 1] = cached_image->data[tile_offset + 1];
            data[atlas_offset + 2] = cached_image->data[tile_offset + 2];
            data[atlas_offset + 3] = cached_image->data[tile_offset + 3];
        }
    }

    auto it = images.find(cached_image);
    if (it == images.end()) {
        atlas_error("Image not found in atlas");
    }

    atlas_tile_t* tile = it->second;
    tile->atlas_id = image.id;
    tile->x1 = (float)tile_offset_x / atlas_dimension;
    tile->y1 = (float)tile_offset_y / atlas_dimension;
    tile->x2 = (float)(tile_offset_x + tile_width) / atlas_dimension;
    tile->y2 = (float)(tile_offset_y + tile_height) / atlas_dimension;
}

atlas_tile_t* atlas_t::add_image(cached_image_t* image_to_add) {
    std::unique_lock<std::mutex> lock(mutex);

    // These are the dimensions that would occur after adding a tile
    uint32_t minimum_x = offset_x + tile_width + padding;
    uint32_t minimum_y = offset_y + tile_height + padding;

    // If the atlas is too small, resize it
    if (minimum_x > atlas_dimension || minimum_y > atlas_dimension) {
        resized = true;
        atlas_dimension *= 2;

        std::vector<uint8_t> new_data;
        new_data.resize(atlas_dimension * atlas_dimension * 4);
        data.swap(new_data);

        offset_x = 0;
        offset_y = 0;

        for (auto& pair : images) {
            images_to_add.push_back(pair.first);
        }
    }

    float current_x = offset_x;
    float current_y = offset_y;

    atlas_tile_t* tile = new atlas_tile_t();
    images[image_to_add] = tile;

    copy_to_data(image_to_add);

    return tile;
}

void atlas_t::upload() {
    std::unique_lock<std::mutex> lock(mutex);
    if (resized) {
        sg_destroy_image(image);
        image.id = SG_INVALID_ID;
    }

    if (image.id == SG_INVALID_ID) {
        sg_image_desc desc = {0};
        desc.type = SG_IMAGETYPE_2D;
        desc.render_target = false;
        desc.width = atlas_dimension;
        desc.height = atlas_dimension;
        desc.num_slices = 1;
        desc.num_mipmaps = 1;
        desc.usage = SG_USAGE_DYNAMIC;
        desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        desc.sample_count = 1;
        desc.min_filter = SG_FILTER_LINEAR;
        desc.mag_filter = SG_FILTER_LINEAR;
        desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        desc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
        desc.border_color = SG_BORDERCOLOR_TRANSPARENT_BLACK;
        desc.max_anisotropy = 1;
        desc.min_lod = 0.0f;
        desc.max_lod = 1e9f;

        image = sg_make_image(desc);

        if (resized) {
            for (cached_image_t* image_to_add : images_to_add) {
                copy_to_data(image_to_add);
            }

            images_to_add.clear();
            resized = false;
        }
    }

    if (dirty) {
        sg_image_data sg_data = {0};
        sg_data.subimage[0][0].ptr = data.data();
        sg_data.subimage[0][0].size = data.size();
        sg_update_image(image, sg_data);
        dirty = false;
    }
}

atlas_tile_t* atlas_map_t::add_image_from_url(const char* url) {
    const std::string url_str(url);
    
}

atlas_map_t* atlas_create_map() {
    return new atlas_map_t();
}

void atlas_destroy_map(atlas_map_t* map) {
    delete map;
}