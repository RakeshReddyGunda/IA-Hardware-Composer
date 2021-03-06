/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "gbmbufferhandler.h"

#include <unistd.h>
#include <drm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include <hwcdefs.h>
#include <hwctrace.h>
#include <platformdefines.h>

#include "commondrmutils.h"
#include "hwcutils.h"

namespace hwcomposer {

// static
NativeBufferHandler *NativeBufferHandler::CreateInstance(uint32_t fd) {
  GbmBufferHandler *handler = new GbmBufferHandler(fd);
  if (!handler)
    return NULL;

  if (!handler->Init()) {
    ETRACE("Failed to initialize GbmBufferHandler.");
    delete handler;
    return NULL;
  }
  return handler;
}

GbmBufferHandler::GbmBufferHandler(uint32_t fd)
    : fd_(fd),
      device_(0),
      preferred_cursor_width_(0),
      preferred_cursor_height_(0) {
}

GbmBufferHandler::~GbmBufferHandler() {
  if (device_)
    gbm_device_destroy(device_);
}

bool GbmBufferHandler::Init() {
  device_ = gbm_create_device(fd_);
  if (!device_) {
    ETRACE("failed to create gbm device \n");
    return false;
  }

  uint64_t width = 0, height = 0;

  if (drmGetCap(fd_, DRM_CAP_CURSOR_WIDTH, &width)) {
    width = 64;
    ETRACE("could not get cursor width.");
  }

  if (drmGetCap(fd_, DRM_CAP_CURSOR_HEIGHT, &height)) {
    height = 64;
    ETRACE("could not get cursor height.");
  }

  preferred_cursor_width_ = width;
  preferred_cursor_height_ = height;

  return true;
}

bool GbmBufferHandler::CreateBuffer(uint32_t w, uint32_t h, int format,
                                    HWCNativeHandle *handle,
                                    uint32_t layer_type, bool *modifier_used,
                                    int64_t preferred_modifier,
                                    bool raw_pixel_buffer) const {
  uint32_t gbm_format = format;
  if (gbm_format == 0)
    gbm_format = GBM_FORMAT_XRGB8888;

  uint32_t flags = 0;

  if (layer_type == kLayerNormal || layer_type == kLayerCursor) {
    flags |= (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
#ifdef USE_MINIGBM
  } else if (layer_type == kLayerVideo) {
    flags |= (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING |
              GBM_BO_USE_CAMERA_WRITE | GBM_BO_USE_CAMERA_READ);
#endif
  }

  if (raw_pixel_buffer) {
    flags |= GBM_BO_USE_LINEAR;
  }

  if (layer_type == kLayerCursor) {
    if (w < preferred_cursor_width_)
      w = preferred_cursor_width_;

    if (h < preferred_cursor_height_)
      h = preferred_cursor_height_;
  }

  struct gbm_bo *bo = NULL;

  bool rbc_enabled = false;
  uint64_t modifier = 0;
#ifdef ENABLE_RBC
  if (preferred_modifier != -1) {
    modifier = preferred_modifier;
  } else {
    modifier = drm_get_modifier(gbm_format);
  }

  if (modifier_used) {
    *modifier_used = true;
  }
#else
  if (modifier_used) {
    *modifier_used = false;
  }
#endif

  if (modifier != DRM_FORMAT_MOD_NONE) {
    bo = gbm_bo_create_with_modifiers(device_, w, h, gbm_format, &modifier, 1);
    rbc_enabled = true;
  } else {
    bo = gbm_bo_create(device_, w, h, gbm_format, flags);
  }

  if (!bo) {
    flags &= ~GBM_BO_USE_SCANOUT;
    bo = gbm_bo_create(device_, w, h, gbm_format, flags);
    rbc_enabled = false;
  }

  if (!bo) {
    flags &= ~GBM_BO_USE_RENDERING;
    bo = gbm_bo_create(device_, w, h, gbm_format, flags);
  }

  if (!bo) {
    ETRACE("GbmBufferHandler: failed to create gbm_bo");
    return false;
  }

  struct gbm_handle *temp = new struct gbm_handle();
  uint64_t mod;
#if USE_MINIGBM
  size_t total_planes = gbm_bo_get_num_planes(bo);
  temp->import_data.width = gbm_bo_get_width(bo);
  temp->import_data.height = gbm_bo_get_height(bo);
  temp->import_data.format = gbm_bo_get_format(bo);

  for (size_t i = 0; i < total_planes; i++) {
    temp->import_data.fds[i] = gbm_bo_get_plane_fd(bo, i);
    temp->import_data.offsets[i] = gbm_bo_get_plane_offset(bo, i);
    temp->import_data.strides[i] = gbm_bo_get_plane_stride(bo, i);
    if (rbc_enabled) {
      mod = gbm_bo_get_plane_format_modifier(bo, i);
      temp->meta_data_.fb_modifiers_[2 * i] = static_cast<uint32_t>(mod >> 32);
      temp->meta_data_.fb_modifiers_[2 * i + 1] = static_cast<uint32_t>(mod);
    }
  }
  temp->meta_data_.num_planes_ = total_planes;
#else
  if (rbc_enabled) {
    temp->import_data.fd_modifier_data.width = gbm_bo_get_width(bo);
    temp->import_data.fd_modifier_data.height = gbm_bo_get_height(bo);
    temp->import_data.fd_modifier_data.format = gbm_bo_get_format(bo);
    temp->import_data.fd_modifier_data.num_fds = 1;
    temp->import_data.fd_modifier_data.fds[0] = gbm_bo_get_fd(bo);

    temp->import_data.fd_modifier_data.modifier = gbm_bo_get_modifier(bo);
    uint32_t modifier_low = static_cast<uint32_t>(
        temp->import_data.fd_modifier_data.modifier >> 32);
    uint32_t modifier_high = static_cast<uint32_t>(mod);

    for (size_t i = 0; i < temp->total_planes; i++) {
      temp->import_data.fd_modifier_data.offsets[i] = gbm_bo_get_offset(bo, i);
      temp->import_data.fd_modifier_data.strides[i] = gbm_bo_get_stride(bo);
      temp->meta_data_.fb_modifiers_[2 * i] = modifier_low;
      temp->meta_data_.fb_modifiers_[2 * i + 1] = modifier_high;
    }
  } else {
    temp->import_data.fd_data.width = gbm_bo_get_width(bo);
    temp->import_data.fd_data.height = gbm_bo_get_height(bo);
    temp->import_data.fd_data.format = gbm_bo_get_format(bo);
    temp->import_data.fd_data.fd = gbm_bo_get_fd(bo);
    temp->import_data.fd_data.stride = gbm_bo_get_stride(bo);
  }
#endif

  temp->bo = bo;
  temp->hwc_buffer_ = true;
  temp->gbm_flags = flags;
  temp->layer_type_ = layer_type;
  *handle = temp;

  return true;
}

bool GbmBufferHandler::ReleaseBuffer(HWCNativeHandle handle) const {
  if (handle->bo || handle->imported_bo) {
    if (handle->bo && handle->hwc_buffer_) {
      gbm_bo_destroy(handle->bo);
    }

    if (handle->imported_bo) {
      gbm_bo_destroy(handle->imported_bo);
    }
#ifdef USE_MINIGBM
    uint32_t total_planes = handle->meta_data_.num_planes_;
    for (size_t i = 0; i < total_planes; i++)
      close(handle->import_data.fds[i]);
#else
    if (!handle->meta_data_.fb_modifiers_[0]) {
      close(handle->import_data.fd_data.fd);
    } else {
      for (size_t i = 0; i < handle->import_data.fd_modifier_data.num_fds; i++)
        close(handle->import_data.fd_modifier_data.fds[i]);
    }
#endif
  }

  return true;
}

void GbmBufferHandler::DestroyHandle(HWCNativeHandle handle) const {
  delete handle;
  handle = NULL;
}

void GbmBufferHandler::CopyHandle(HWCNativeHandle source,
                                  HWCNativeHandle *target) const {
  struct gbm_handle *temp = new struct gbm_handle();

#if USE_MINIGBM
  temp->import_data.width = source->import_data.width;
  temp->import_data.height = source->import_data.height;
  temp->import_data.format = source->import_data.format;

  size_t total_planes = source->meta_data_.num_planes_;
  for (size_t i = 0; i < total_planes; i++) {
    temp->import_data.fds[i] = dup(source->import_data.fds[i]);
    temp->import_data.offsets[i] = source->import_data.offsets[i];
    temp->import_data.strides[i] = source->import_data.strides[i];
    temp->meta_data_.fb_modifiers_[i] = source->meta_data_.fb_modifiers_[i];
    temp->meta_data_.fb_modifiers_[2 * i + 1] =
        source->meta_data_.fb_modifiers_[2 * i + 1];
  }
#else
  if (!source->meta_data_.fb_modifiers_[0]) {
    temp->import_data.fd_data.width = source->import_data.fd_data.width;
    temp->import_data.fd_data.height = source->import_data.fd_data.height;
    temp->import_data.fd_data.format = source->import_data.fd_data.format;
    temp->import_data.fd_data.fd = dup(source->import_data.fd_data.fd);
    temp->import_data.fd_data.stride = source->import_data.fd_data.stride;
  } else {
    temp->import_data.fd_modifier_data.width =
        source->import_data.fd_modifier_data.width;
    temp->import_data.fd_modifier_data.height =
        source->import_data.fd_modifier_data.height;
    temp->import_data.fd_modifier_data.format =
        source->import_data.fd_modifier_data.format;
    temp->total_planes = source->total_planes;
    temp->import_data.fd_modifier_data.num_fds =
        source->import_data.fd_modifier_data.num_fds;

    for (size_t i = 0; i < temp->import_data.fd_modifier_data.num_fds; i++) {
      temp->import_data.fd_modifier_data.fds[i] =
          dup(source->import_data.fd_modifier_data.fds[i]);
    }

    for (size_t i = 0; i < temp->total_planes; i++) {
      temp->import_data.fd_modifier_data.offsets[i] =
          source->import_data.fd_modifier_data.offsets[i];
      temp->import_data.fd_modifier_data.strides[i] =
          source->import_data.fd_modifier_data.strides[i];
      temp->meta_data_.fb_modifiers_[2 * i] =
          source->meta_data_.fb_modifiers_[2 * i];
      temp->meta_data_.fb_modifiers_[2 * i + 1] =
          source->meta_data_.fb_modifiers_[2 * i + 1];
    }

    temp->import_data.fd_modifier_data.modifier =
        source->import_data.fd_modifier_data.modifier;
  }
#endif
  temp->bo = source->bo;
  temp->meta_data_.num_planes_ = source->meta_data_.num_planes_;
  temp->gbm_flags = source->gbm_flags;
  temp->layer_type_ = source->layer_type_;
  *target = temp;
}

bool GbmBufferHandler::ImportBuffer(HWCNativeHandle handle) const {
  memset(&(handle->meta_data_), 0, sizeof(struct HwcBuffer));
  uint32_t gem_handle = 0;
  HwcBuffer *bo = &(handle->meta_data_);
  bool use_modifer = true;
  uint64_t mod = 0;

  if (!handle->imported_bo) {
#if USE_MINIGBM
    bo->format_ = handle->import_data.format;
    bo->native_format_ = handle->import_data.format;
    handle->imported_bo =
        gbm_bo_import(device_, GBM_BO_IMPORT_FD_PLANAR, &handle->import_data,
                      handle->gbm_flags);

    if (!handle->imported_bo) {
      ETRACE("can't import bo");
    }
#else
    if (!handle->meta_data_.fb_modifiers_[0]) {
      use_modifer = false;
      bo->format_ = handle->import_data.fd_data.format;
      handle->imported_bo =
          gbm_bo_import(device_, GBM_BO_IMPORT_FD, &handle->import_data.fd_data,
                        handle->gbm_flags);
    } else {
      bo->format_ = handle->import_data.fd_modifier_data.format;
      handle->imported_bo = gbm_bo_import(device_, GBM_BO_IMPORT_FD_MODIFIER,
                                          &handle->import_data.fd_modifier_data,
                                          handle->gbm_flags);
    }

    if (!handle->imported_bo) {
      ETRACE("can't import bo");
    }
#endif
  }

  gem_handle = gbm_bo_get_handle(handle->imported_bo).u32;

  if (!gem_handle) {
    ETRACE("Invalid GEM handle. \n");
    return false;
  }

#if USE_MINIGBM
  handle->meta_data_.width_ = handle->import_data.width;
  handle->meta_data_.height_ = handle->import_data.height;
#else
  if (!use_modifer) {
    handle->meta_data_.width_ = handle->import_data.fd_data.width;
    handle->meta_data_.height_ = handle->import_data.fd_data.height;
  } else {
    handle->meta_data_.width_ = handle->import_data.fd_modifier_data.width;
    handle->meta_data_.height_ = handle->import_data.fd_modifier_data.height;
  }
#endif

  if (handle->layer_type_ == hwcomposer::kLayerCursor) {
    handle->meta_data_.usage_ = hwcomposer::kLayerCursor;
    // We support DRM_FORMAT_ARGB8888 for cursor.
    handle->meta_data_.format_ = DRM_FORMAT_ARGB8888;
  } else if (hwcomposer::IsSupportedMediaFormat(handle->meta_data_.format_)) {
    handle->meta_data_.usage_ = hwcomposer::kLayerVideo;
  } else {
    handle->meta_data_.usage_ = hwcomposer::kLayerNormal;
  }

#if USE_MINIGBM
  size_t total_planes = gbm_bo_get_num_planes(handle->bo);
  handle->meta_data_.num_planes_ = total_planes;
  for (size_t i = 0; i < total_planes; i++) {
    handle->meta_data_.gem_handles_[i] = gem_handle;
    handle->meta_data_.offsets_[i] =
        gbm_bo_get_plane_offset(handle->imported_bo, i);
    handle->meta_data_.pitches_[i] =
        gbm_bo_get_plane_stride(handle->imported_bo, i);
    handle->meta_data_.prime_fds_[i] = handle->import_data.fds[i];
    mod = gbm_bo_get_plane_format_modifier(handle->bo, i);
    bo->fb_modifiers_[2 * i] = static_cast<uint32_t>(mod >> 32);
    bo->fb_modifiers_[2 * i + 1] = static_cast<uint32_t>(mod);
  }
#else
  if (!use_modifer) {
    bo->prime_fds_[0] = handle->import_data.fd_data.fd;
  } else {
    bo->prime_fds_[0] = handle->import_data.fd_modifier_data.fds[0];
  }

  size_t total_planes = gbm_bo_get_plane_count(handle->imported_bo);
  bo->num_planes_ = total_planes;
  mod = gbm_bo_get_modifier(handle->imported_bo);
  uint32_t modifier_low = static_cast<uint32_t>(
      handle->import_data.fd_modifier_data.modifier >> 32);
  uint32_t modifier_high = static_cast<uint32_t>(mod);

  for (size_t i = 0; i < total_planes; i++) {
    bo->gem_handles_[i] =
        gbm_bo_get_handle_for_plane(handle->imported_bo, i).u32;
    bo->offsets_[i] = handle->import_data.fd_modifier_data.offsets[i];
    bo->pitches_[i] = gbm_bo_get_stride_for_plane(handle->imported_bo, i);
    bo->fb_modifiers_[2 * i] = modifier_low;
    bo->fb_modifiers_[2 * i + 1] = modifier_high;
  }
#endif

  return true;
}

uint32_t GbmBufferHandler::GetTotalPlanes(HWCNativeHandle handle) const {
  return handle->meta_data_.num_planes_;
}

void *GbmBufferHandler::Map(HWCNativeHandle handle, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, uint32_t *stride,
                            void **map_data, size_t plane) const {
  if (!handle->bo)
    return NULL;

#if USE_MINIGBM
  return gbm_bo_map(handle->bo, x, y, width, height, GBM_BO_TRANSFER_WRITE,
                    stride, map_data, plane);
#else
  return gbm_bo_map(handle->bo, x, y, width, height, GBM_BO_TRANSFER_WRITE,
                    stride, map_data);
#endif
}

int32_t GbmBufferHandler::UnMap(HWCNativeHandle handle, void *map_data) const {
  if (!handle->bo)
    return -1;

  gbm_bo_unmap(handle->bo, map_data);
  return 0;
}

}  // namespace hwcomposer
