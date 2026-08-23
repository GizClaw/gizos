#include "h2_game_runtime_internal.hpp"

#include "graphics/DisplayConfig.h"

#include <new>

int h2_game_runtime_create(const h2_game_runtime_config_t *config, h2_game_runtime_t **out_runtime) {
    if (config == nullptr || out_runtime == nullptr || config->display == nullptr ||
        config->width <= 0 || config->height <= 0 || config->scene == nullptr) {
        return H2_GAME_RUNTIME_ERR_INVALID_ARG;
    }

    *out_runtime = nullptr;

    if (h2_pal_display_open(config->display) != H2_DISPLAY_OK) {
        return H2_GAME_RUNTIME_ERR_DISPLAY;
    }

    auto *runtime = new (std::nothrow) h2_game_runtime;
    if (runtime == nullptr) {
        return H2_GAME_RUNTIME_ERR_NO_MEMORY;
    }

    auto *surface = new (std::nothrow) H2PixelRootDisplaySurface(config->display, config->width, config->height);
    if (surface == nullptr) {
        delete runtime;
        return H2_GAME_RUNTIME_ERR_NO_MEMORY;
    }
    if (surface->getPixelBuffer() == nullptr) {
        delete surface;
        delete runtime;
        return H2_GAME_RUNTIME_ERR_NO_MEMORY;
    }

    runtime->display = config->display;
    runtime->width = config->width;
    runtime->height = config->height;
    runtime->scene = static_cast<pixelroot32::core::Scene *>(config->scene);
    runtime->input_handler = config->input_handler;

    pixelroot32::graphics::DisplayConfig display_config =
        pixelroot32::graphics::DisplayConfig::createCustom(surface, static_cast<uint16_t>(config->width), static_cast<uint16_t>(config->height));
    auto *renderer = new (std::nothrow) pixelroot32::graphics::Renderer(std::move(display_config));
    if (renderer == nullptr) {
        delete runtime;
        return H2_GAME_RUNTIME_ERR_NO_MEMORY;
    }
    runtime->renderer.reset(renderer);

    runtime->renderer->init();
    runtime->scene->init();

    *out_runtime = runtime;
    return H2_GAME_RUNTIME_OK;
}

int h2_game_runtime_tick(h2_game_runtime_t *runtime, uint32_t now_ms) {
    if (runtime == nullptr) {
        return H2_GAME_RUNTIME_ERR_INVALID_ARG;
    }

    uint32_t delta_ms = 0;
    if (runtime->has_last_tick) {
        delta_ms = now_ms - runtime->last_tick_ms;
    } else {
        runtime->has_last_tick = true;
    }
    runtime->last_tick_ms = now_ms;

    h2_game_runtime_drain_input(runtime);

    runtime->scene->update(delta_ms);
    runtime->renderer->beginFrame();
    runtime->scene->draw(*runtime->renderer);
    runtime->renderer->endFrame();

    auto *surface = static_cast<H2PixelRootDisplaySurface *>(&runtime->renderer->getDrawSurface());
    return surface->last_status();
}

void h2_game_runtime_destroy(h2_game_runtime_t *runtime) {
    if (runtime == nullptr) {
        return;
    }
    delete runtime;
}
