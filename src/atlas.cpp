#include "atlas.h"
#include "https.hpp"
#include "sokol_gfx.h"
#include "stb_image.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

[[noreturn]] void atlas_error(const char* message) {
    fprintf(stderr, "Atlas error: %s\n", message);
    exit(1);
}

struct cached_image_t {
    const uint8_t* data = nullptr;
    uint32_t width, height;
};

struct atlas_tile_t {
    std::atomic_uint32_t atlas_id;
    std::atomic<float> x1, y1, x2, y2;
};

std::mutex image_cache_mutex;
std::unordered_map<std::string, cached_image_t*> image_cache;

std::mutex atlas_maps_mutex;
std::vector<atlas_map_t*> atlas_maps;

std::mutex to_delete_mutex;
std::vector<sg_image> images_to_delete;

struct atlas_t {
    atlas_t(uint32_t tile_width, uint32_t tile_height);
    ~atlas_t();

    atlas_tile_t* get_tile(const std::string& url) {
        std::unique_lock<std::mutex> lock(mutex);
        if (images.find(url) == images.end()) {
            atlas_error("Tile not found");
        }

        atlas_tile_t* tile = images[url];
        return tile;
    }
    bool has_tile(const std::string& url) {
        std::unique_lock<std::mutex> lock(mutex);
        return images.find(url) != images.end();
    }
    void add_tile(const std::string& url, atlas_tile_t* tile, cached_image_t* cached_image);
    void upload();

private:
    void copy_to_data(atlas_tile_t* tile, cached_image_t* image);

    const uint32_t tile_width, tile_height;

    std::mutex mutex;
    sg_image atlas_image;
    uint32_t atlas_dimension;
    uint32_t offset_x, offset_y;
    std::vector<uint8_t> data;
    std::unordered_map<std::string, atlas_tile_t*> images;
    std::vector<std::string> images_to_add;
    bool dirty;   // new data needs to be uploaded to the GPU
    bool resized; // atlas needs to be destroyed and created at new size

    constexpr static uint32_t padding = 4;
};

struct atlas_map_t {
    atlas_tile_t* add_tile_from_url(const char* url);
    atlas_tile_t* add_tile_from_path(const char* path);
    void wait_all();
    void upload_all();

    std::atomic_int requests = {0};

private:
    atlas_t* get_atlas(uint32_t tile_width, uint32_t tile_height) {
        std::unique_lock<std::mutex> lock(atlases_mutex);
        uint32_t key = tile_width << 16 | tile_height;

        atlas_t* atlas = atlases[key];

        if (atlas == nullptr) {
            atlas = new atlas_t(tile_width, tile_height);
            atlases[key] = atlas;
        }

        return atlas;
    }

    std::mutex atlases_mutex;
    std::mutex adding_mutex;
    std::unordered_map<uint32_t, atlas_t*> atlases;
};

atlas_t::atlas_t(uint32_t tile_width, uint32_t tile_height) : tile_width(tile_width), tile_height(tile_height) {
    atlas_image.id = SG_INVALID_ID;
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

atlas_t::~atlas_t() {
    for (auto& pair : images) {
        delete pair.second;
    }

    std::unique_lock<std::mutex> lock(to_delete_mutex);
    images_to_delete.push_back(atlas_image);
}

void atlas_t::copy_to_data(atlas_tile_t* tile, cached_image_t* cached_image) {
    if (tile == nullptr) {
        atlas_error("Tile is null");
    }

    if (cached_image->data == nullptr) {
        atlas_error("Cached image data is null");
    }

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

    tile->atlas_id = atlas_image.id;
    tile->x1 = (float)tile_offset_x / atlas_dimension;
    tile->y1 = (float)tile_offset_y / atlas_dimension;
    tile->x2 = (float)(tile_offset_x + tile_width) / atlas_dimension;
    tile->y2 = (float)(tile_offset_y + tile_height) / atlas_dimension;
}

void atlas_t::add_tile(const std::string& url, atlas_tile_t* tile, cached_image_t* cached_image) {
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

    images[url] = tile;

    copy_to_data(tile, cached_image);
}

void atlas_t::upload() {
    std::unique_lock<std::mutex> lock(mutex);
    if (resized) {
        sg_destroy_image(atlas_image);
        atlas_image.id = SG_INVALID_ID;
    }

    if (atlas_image.id == SG_INVALID_ID) {
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

        atlas_image = sg_make_image(desc);

        if (resized) {
            {
                std::unique_lock<std::mutex> lock(image_cache_mutex);
                for (const std::string& image_url : images_to_add) {
                    cached_image_t* cached_image = image_cache[image_url];
                    atlas_tile_t* tile = images[image_url];
                    copy_to_data(tile, cached_image);
                }
            }

            images_to_add.clear();

            resized = false;
        }

        for (auto& pair : images) {
            atlas_tile_t* tile = pair.second;
            tile->atlas_id = atlas_image.id;
        }
    }

    if (dirty) {
        sg_image_data sg_data = {0};
        sg_data.subimage[0][0].ptr = data.data();
        sg_data.subimage[0][0].size = data.size();
        sg_update_image(atlas_image, sg_data);
        dirty = false;
    }
}

atlas_tile_t* atlas_map_t::add_tile_from_url(const char* url) {
    std::unique_lock<std::mutex> lock(adding_mutex);
    const std::string url_str(url);

    {
        std::unique_lock<std::mutex> lock(image_cache_mutex);
        if (image_cache.find(url_str) != image_cache.end()) {
            cached_image_t* cached_image = image_cache[url_str];
            lock.unlock();

            atlas_t* atlas = get_atlas(cached_image->width, cached_image->height);
            atlas_tile_t* tile = nullptr;
            if (atlas->has_tile(url_str)) {
                tile = atlas->get_tile(url_str);
            } else {
                tile = new atlas_tile_t();
                atlas->add_tile(url_str, tile, cached_image);
            }

            if (tile == nullptr) {
                atlas_error("Tile is null");
            }

            return tile;
        }
    }

    atlas_tile_t* tile = new atlas_tile_t();

    requests++;
    https_request(http_request_e::GET, url_str, {}, {}, [this, url_str, tile] (const std::vector<uint8_t>& result) {
        if (result.empty()) {
            printf("Failed to download image for atlas\n");
            delete tile;
            requests--;
            return;
        }

        cached_image_t* cached_image = new cached_image_t();
        int width, height;
        cached_image->data = stbi_load_from_memory(result.data(), result.size(), &width, &height, NULL, 4);
        cached_image->width = width;
        cached_image->height = height;

        if (!cached_image->data)
        {
            printf("Failed to load image for atlas\n");
            delete tile;
            delete cached_image;
        } else {
            {
                std::unique_lock<std::mutex> lock(image_cache_mutex);
                image_cache[url_str] = cached_image;
            }

            atlas_t* atlas = get_atlas(cached_image->width, cached_image->height);
            atlas->add_tile(url_str, tile, cached_image);
        }

        requests--;
    });

    return tile;
}

atlas_tile_t* atlas_map_t::add_tile_from_path(const char* path) {
    std::unique_lock<std::mutex> lock(adding_mutex);
    atlas_error("Not implemented");
    return nullptr;
}

void atlas_map_t::wait_all() {
    // this mutex solely exists to wait for the add_* functions to finish and begin their requests
    // it's realistically very unlikely anything bad would happen without it but possible
    std::unique_lock<std::mutex> lock(adding_mutex);
    while (requests > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void atlas_map_t::upload_all() {
    std::unique_lock<std::mutex> lock(atlases_mutex);
    for (auto& pair : atlases) {
        pair.second->upload();
    }
}

atlas_map_t* atlas_create_map() {
    atlas_map_t* map = new atlas_map_t();
    {
        std::unique_lock<std::mutex> lock(atlas_maps_mutex);
        atlas_maps.push_back(map);
    }
    return map;
}

void atlas_destroy_map(atlas_map_t* map) {
    {
        std::unique_lock<std::mutex> lock(atlas_maps_mutex);
        auto it = std::find(atlas_maps.begin(), atlas_maps.end(), map);
        if (it != atlas_maps.end()) {
            atlas_maps.erase(it);
        }
    }

    std::thread delete_thread([map] {
        map->wait_all();
        delete map;
    });
    delete_thread.detach();
}

atlas_tile_t* atlas_add_tile_from_url(atlas_map_t* map, const char* url) {
    if (map == nullptr) {
        atlas_error("Map is null");
    }

    return map->add_tile_from_url(url);
}

atlas_tile_t* atlas_add_tile_from_path(atlas_map_t* map, const char* path) {
    atlas_error("Not implemented");
    return nullptr;
}

void atlas_upload_all() {
    std::unique_lock<std::mutex> lock(atlas_maps_mutex);
    for (atlas_map_t* map : atlas_maps) {
        if (map->requests > 0) {
            continue; // probably a lot of outstanding requests and we don't wanna update too often
        }

        map->upload_all();
    }
    lock.unlock();

    std::unique_lock<std::mutex> dlock(to_delete_mutex);
    for (sg_image image : images_to_delete) {
        sg_destroy_image(image);
    }
    images_to_delete.clear();       
}

uint32_t atlas_get_tile_id(atlas_tile_t* tile) {
    if (tile == nullptr) {
        return 0;
    }

    return tile->atlas_id;
}

atlas_uvs_t atlas_get_tile_uvs(atlas_tile_t* tile) {
    return {tile->x1, tile->y1, tile->x2, tile->y2};
}