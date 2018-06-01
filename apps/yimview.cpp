//
// LICENSE:
//
// Copyright (c) 2016 -- 2018 Fabio Pellacini
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include "../yocto/yocto_glutils.h"
#include "../yocto/yocto_image.h"
#include "../yocto/yocto_utils.h"

#include <unordered_map>

// Generic image that contains either an HDR or an LDR image, giving access
// to both. This is helpful when writing viewers or generic image
// manipulation code
struct gimage {
    std::string name;
    std::string filename;  // path

    int width = 0;                // width
    int height = 0;               // height
    std::vector<ygl::vec4f> img;  // pixels

    // min/max values
    ygl::bbox4f pxl_bounds = ygl::invalid_bbox4f;
    ygl::bbox1f lum_bounds = ygl::invalid_bbox1f;

    // image adjustment
    bool updated = true;
    float exposure = 0;
    float gamma = 2.2f;
    bool filmic = false;

    // display image
    std::vector<ygl::vec4f> display;

    // opengl texture
    bool gl_updated = true;
    ygl::gltexture gl_txt;
};

struct app_state {
    std::vector<gimage*> imgs;
    gimage* img = nullptr;

    ygl::glimage_program gl_prog = {};
    ygl::frame2f imframe = ygl::identity_frame2f;
    bool zoom_to_fit = false;
    ygl::vec4f background = {0.8f, 0.8f, 0.8f, 0};

    ~app_state() {
        for (auto v : imgs) delete v;
    }
};

// compute min/max
void update_minmax(gimage* img) {
    img->pxl_bounds = ygl::invalid_bbox4f;
    img->lum_bounds = ygl::invalid_bbox1f;
    for (auto& p : img->img) {
        img->pxl_bounds += p;
        img->lum_bounds += ygl::luminance(xyz(p));
    }
}

// Loads a generic image
gimage* load_gimage(const std::string& filename, float exposure, float gamma) {
    auto img = new gimage();
    img->filename = filename;
    img->name = ygl::path_filename(filename);
    img->exposure = exposure;
    img->gamma = gamma;
    img->img = ygl::load_image(filename, img->width, img->height, img->gamma);
    if (img->img.empty()) {
        throw std::runtime_error("cannot load image " + img->filename);
    }
    update_minmax(img);
    return img;
}

gimage* diff_gimage(gimage* a, gimage* b, bool color) {
    if (a->width != b->width || a->height != b->height) return nullptr;
    if (a->img.empty() && !b->img.empty()) return nullptr;
    auto d = new gimage();
    d->name = "diff " + a->name + " " + b->name;
    d->filename = "";
    d->width = a->width;
    d->height = a->height;
    if (!a->img.empty()) {
        d->img.resize(a->img.size());
        if (color) {
            for (auto i = 0; i < a->img.size(); i++) {
                d->img[i] = {std::abs(a->img[i].x - b->img[i].x),
                    std::abs(a->img[i].y - b->img[i].y),
                    std::abs(a->img[i].z - b->img[i].z),
                    std::max(a->img[i].w, b->img[i].w)};
            }
        } else {
            for (auto i = 0; i < a->img.size(); i++) {
                auto la = (a->img[i].x + a->img[i].y + a->img[i].z) / 3;
                auto lb = (b->img[i].x + b->img[i].y + b->img[i].z) / 3;
                auto ld = fabsf(la - lb);
                d->img[i] = {ld, ld, ld, std::max(a->img[i].w, b->img[i].w)};
            }
        }
    }
    update_minmax(d);
    return d;
}

void update_display_image(gimage* img) {
    img->display = img->img;
    if (img->exposure)
        img->display = ygl::expose_image(img->display, img->exposure);
    if (img->filmic) img->display = ygl::filmic_tonemap_image(img->display);
    if (img->gamma != 1)
        img->display = ygl::linear_to_gamma(img->display, img->gamma);
    img->updated = false;
}

void draw_glwidgets(ygl::glwindow* win, app_state* app) {
    if (ygl::begin_glwidgets_frame(win, "yimview")) {
        ygl::draw_glwidgets_combobox(win, "image", app->img, app->imgs);
        ygl::draw_glwidgets_label(win, "filename", app->img->filename);
        ygl::draw_glwidgets_label(win, "size",
            ygl::format("{} x {}", app->img->width, app->img->height));
        auto edited = 0;
        edited += ygl::draw_glwidgets_dragbox(
            win, "exposure", app->img->exposure, -5, 5);
        edited +=
            ygl::draw_glwidgets_dragbox(win, "gamma", app->img->gamma, 1, 3);
        edited += ygl::draw_glwidgets_checkbox(win, "filmic", app->img->filmic);
        if (edited) app->img->updated = true;
        auto zoom = app->imframe.x.x;
        if (ygl::draw_glwidgets_dragbox(win, "zoom", zoom, 0.1, 10))
            app->imframe.x.x = app->imframe.y.y = zoom;
        ygl::draw_glwidgets_checkbox(win, "zoom to fit", app->zoom_to_fit);
        ygl::draw_glwidgets_colorbox(win, "background", app->background);
        ygl::draw_glwidgets_separator(win);
        auto ij = ygl::get_glimage_coords(get_glwidnow_mouse_posf(win),
            app->imframe, {app->img->width, app->img->height});
        ygl::draw_glwidgets_dragbox(win, "mouse", ij);
        auto pixel = ygl::zero4f;
        if (ij.x >= 0 && ij.x < app->img->width && ij.y >= 0 &&
            ij.y < app->img->height) {
            pixel = app->img->img.at(ij.x + ij.y * app->img->width);
        }
        ygl::draw_glwidgets_colorbox(win, "pixel", pixel);
        if (!app->img->img.empty()) {
            ygl::draw_glwidgets_dragbox(win, "pxl", app->img->pxl_bounds);
            ygl::draw_glwidgets_dragbox(
                win, "lum min/max", app->img->lum_bounds);
        }
    }
    ygl::end_glwidgets_frame(win);
}

void draw(ygl::glwindow* win, app_state* app) {
    auto window_size = get_glwindow_size(win);
    auto framebuffer_size = get_glwindow_framebuffer_size(win);
    ygl::set_glviewport(framebuffer_size);
    ygl::clear_glbuffers(app->background);
    ygl::draw_glimage(
        app->gl_prog, app->img->gl_txt, window_size, app->imframe);
    draw_glwidgets(win, app);
    ygl::swap_glwindow_buffers(win);
}

void refresh(ygl::glwindow* win) {
    auto app = (app_state*)ygl::get_glwindow_user_pointer(win);
    ygl::center_glimage(app->imframe, {app->img->width, app->img->height},
        ygl::get_glwindow_size(win), app->zoom_to_fit);
    draw(win, app);
}

void run_ui(app_state* app) {
    // window
    auto win_width = app->imgs[0]->width + ygl::default_glwidgets_width;
    auto win_height = ygl::clamp(app->imgs[0]->height, 512, 1024);
    auto win = ygl::make_glwindow(win_width, win_height, "yimview", app);
    ygl::set_glwindow_callbacks(win, nullptr, nullptr, refresh);
    ygl::center_glimage(app->imframe,
        {app->imgs[0]->width, app->imgs[0]->height},
        ygl::get_glwindow_size(win), app->zoom_to_fit);

    // window values
    int mouse_button = 0;
    ygl::vec2f mouse_pos, mouse_last;

    // init widgets
    ygl::init_glwidgets(win);

    // load textures
    app->gl_prog = ygl::make_glimage_program();

    while (!should_glwindow_close(win)) {
        mouse_last = mouse_pos;
        mouse_pos = ygl::get_glwidnow_mouse_posf(win);
        mouse_button = ygl::get_glwindow_mouse_button(win);

        ygl::set_glwindow_title(
            win, ygl::format("yimview | {} | {}x{}", app->img->filename,
                     app->img->width, app->img->height));

        // handle mouse
        auto alt_down = get_glwindow_alt_key(win);
        if (mouse_button && alt_down && mouse_pos != mouse_last &&
            !ygl::get_glwidgets_active(win)) {
            switch (mouse_button) {
                case 1: app->imframe.o += mouse_pos - mouse_last; break;
                case 2: {
                    auto zoom = powf(2, (mouse_pos.x - mouse_last.x) * 0.001f);
                    app->imframe = app->imframe *
                                   ygl::frame2f{{zoom, 0}, {0, zoom}, {0, 0}};
                } break;
                default: break;
            }
        }
        // auto center
        ygl::center_glimage(app->imframe, {app->img->width, app->img->height},
            ygl::get_glwindow_size(win), app->zoom_to_fit);

        // update texture
        if (app->img->updated) {
            update_display_image(app->img);
            app->img->gl_updated = true;
        }
        if (app->img->gl_updated) {
            update_gltexture(app->img->gl_txt, app->img->width,
                app->img->height, app->img->display, false, false, true, false);
            app->img->gl_updated = false;
        }

        // draw
        draw(win, app);

        // event hadling
        if (ygl::get_glwindow_mouse_button(win) ||
            ygl::get_glwidgets_active(win)) {
            ygl::poll_glwindow_events(win);
        } else {
            ygl::wait_glwindow_events(win);
        }
    }

    // cleanup
    delete win;
}

int main(int argc, char* argv[]) {
    auto app = new app_state();

    // command line params
    auto parser = ygl::make_parser(argc, argv, "yimview", "view images");
    auto gamma = ygl::parse_opt(parser, "--gamma", "-g", "display gamma", 2.2f);
    auto exposure =
        ygl::parse_opt(parser, "--exposure", "-e", "display exposure", 0.0f);
    auto diff = ygl::parse_flag(parser, "--diff", "-d", "compute diff images");
    auto lum_diff = ygl::parse_flag(
        parser, "--luminance-diff", "-D", "compute luminance diffs");
    auto filenames = ygl::parse_args(
        parser, "image", "image filename", std::vector<std::string>{});
    // check parsing
    if (ygl::should_exit(parser)) {
        printf("%s\n", get_usage(parser).c_str());
        exit(1);
    }

    // loading images
    for (auto filename : filenames) {
        ygl::log_info("loading {}", filename);
        app->imgs.push_back(load_gimage(filename, exposure, gamma));
    }
    app->img = app->imgs.at(0);
    if (diff) {
        auto num = app->imgs.size();
        for (auto i = 0; i < num; i++) {
            for (auto j = i + 1; j < num; j++) {
                ygl::log_info("diffing {} {}", app->imgs[i]->filename,
                    app->imgs[j]->filename);
                auto diff = diff_gimage(app->imgs[i], app->imgs[j], !lum_diff);
                if (diff) app->imgs.push_back(diff);
            }
        }
    }

    // run ui
    run_ui(app);

    // cleanup
    delete app;

    // done
    return 0;
}
