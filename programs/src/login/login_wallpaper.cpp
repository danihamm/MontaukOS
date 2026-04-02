/*
    * login_wallpaper.cpp
    * Wallpaper loading for the MontaukOS login screen
    * Copyright (c) 2026 Daniel Hammer
*/

#include "login.hpp"

bool load_login_wallpaper(LoginState* ls) {
    auto doc = montauk::config::load("desktop");
    const char* wp = doc.get_string("wallpaper.path", "");
    if (wp[0] == '\0') return false;

    int fd = montauk::open(wp);
    if (fd < 0) return false;

    uint64_t size = montauk::getsize(fd);
    if (size == 0 || size > 16 * 1024 * 1024) {
        montauk::close(fd);
        return false;
    }

    uint8_t* filedata = (uint8_t*)montauk::malloc(size);
    if (!filedata) {
        montauk::close(fd);
        return false;
    }

    int bytes_read = montauk::read(fd, filedata, 0, size);
    montauk::close(fd);
    if (bytes_read <= 0) {
        montauk::mfree(filedata);
        return false;
    }

    int img_w, img_h, channels;
    unsigned char* rgb = stbi_load_from_memory(filedata, bytes_read,
                                               &img_w, &img_h, &channels, 3);
    montauk::mfree(filedata);
    if (!rgb) return false;

    int dst_w = ls->screen_w;
    int dst_h = ls->screen_h;
    uint32_t* scaled = (uint32_t*)montauk::malloc((uint64_t)dst_w * dst_h * 4);
    if (!scaled) {
        stbi_image_free(rgb);
        return false;
    }

    int src_crop_w, src_crop_h, src_x0, src_y0;
    if ((int64_t)img_w * dst_h > (int64_t)img_h * dst_w) {
        src_crop_h = img_h;
        src_crop_w = (int)((int64_t)img_h * dst_w / dst_h);
        src_x0 = (img_w - src_crop_w) / 2;
        src_y0 = 0;
    } else {
        src_crop_w = img_w;
        src_crop_h = (int)((int64_t)img_w * dst_h / dst_w);
        src_x0 = 0;
        src_y0 = (img_h - src_crop_h) / 2;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = src_y0 + (int)((int64_t)y * src_crop_h / dst_h);
        if (sy < 0) sy = 0;
        if (sy >= img_h) sy = img_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = src_x0 + (int)((int64_t)x * src_crop_w / dst_w);
            if (sx < 0) sx = 0;
            if (sx >= img_w) sx = img_w - 1;
            int si = (sy * img_w + sx) * 3;
            scaled[y * dst_w + x] = 0xFF000000u
                | ((uint32_t)rgb[si] << 16)
                | ((uint32_t)rgb[si + 1] << 8)
                | (uint32_t)rgb[si + 2];
        }
    }

    stbi_image_free(rgb);
    ls->bg_wallpaper = scaled;
    ls->bg_wallpaper_w = dst_w;
    ls->bg_wallpaper_h = dst_h;
    ls->has_wallpaper = true;
    return true;
}
