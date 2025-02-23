/*
* Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Changes from Qualcomm Innovation Center are provided under the following license:
Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted (subject to the limitations in the
disclaimer below) provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of Qualcomm Innovation Center, Inc. nor the
      names of its contributors may be used to endorse or promote
      products derived from this software without specific prior
      written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <cutils/properties.h>
#include <sync/sync.h>
#include <utils/constants.h>
#include <utils/debug.h>
#include <utils/utils.h>
#include <stdarg.h>
#include <sys/mman.h>

#include <map>
#include <string>
#include <vector>

#include "hwc_display_builtin.h"
#include "hwc_color_mode_stc.h"
#include "hwc_debugger.h"
#include "hwc_session.h"

#define __CLASS__ "HWCDisplayBuiltIn"

namespace sdm {

static void SetRect(LayerRect &src_rect, GLRect *target) {
  target->left = src_rect.left;
  target->top = src_rect.top;
  target->right = src_rect.right;
  target->bottom = src_rect.bottom;
}

int HWCDisplayBuiltIn::Create(CoreInterface *core_intf, BufferAllocator *buffer_allocator,
                              HWCCallbacks *callbacks, HWCDisplayEventHandler *event_handler,
                              qService::QService *qservice, hwc2_display_t id, int32_t sdm_id,
                              HWCDisplay **hwc_display) {
  int status = 0;
  uint32_t builtin_width = 0;
  uint32_t builtin_height = 0;

  HWCDisplay *hwc_display_builtin =
      new HWCDisplayBuiltIn(core_intf, buffer_allocator, callbacks, event_handler, qservice, id,
                            sdm_id);
  status = hwc_display_builtin->Init();
  if (status) {
    delete hwc_display_builtin;
    return status;
  }

  hwc_display_builtin->GetMixerResolution(&builtin_width, &builtin_height);
  int width = 0, height = 0;
  HWCDebugHandler::Get()->GetProperty(FB_WIDTH_PROP, &width);
  HWCDebugHandler::Get()->GetProperty(FB_HEIGHT_PROP, &height);
  if (width > 0 && height > 0) {
    builtin_width = UINT32(width);
    builtin_height = UINT32(height);
  }

  status = hwc_display_builtin->SetFrameBufferResolution(builtin_width, builtin_height);
  if (status) {
    Destroy(hwc_display_builtin);
    return status;
  }

  *hwc_display = hwc_display_builtin;

  return status;
}

void HWCDisplayBuiltIn::Destroy(HWCDisplay *hwc_display) {
  hwc_display->Deinit();
  delete hwc_display;
}

HWCDisplayBuiltIn::HWCDisplayBuiltIn(CoreInterface *core_intf, BufferAllocator *buffer_allocator,
                                     HWCCallbacks *callbacks, HWCDisplayEventHandler *event_handler,
                                     qService::QService *qservice, hwc2_display_t id,
                                     int32_t sdm_id)
    : HWCDisplay(core_intf, buffer_allocator, callbacks, event_handler, qservice, kBuiltIn, id,
                 sdm_id, DISPLAY_CLASS_BUILTIN),
      buffer_allocator_(buffer_allocator),
      cpu_hint_(NULL), layer_stitch_task_(*this) {
}

int HWCDisplayBuiltIn::Init() {
  cpu_hint_ = new CPUHint();
  if (cpu_hint_->Init(static_cast<HWCDebugHandler *>(HWCDebugHandler::Get())) != kErrorNone) {
    delete cpu_hint_;
    cpu_hint_ = NULL;
  }

  use_metadata_refresh_rate_ = true;
  int disable_metadata_dynfps = 0;
  HWCDebugHandler::Get()->GetProperty(DISABLE_METADATA_DYNAMIC_FPS_PROP, &disable_metadata_dynfps);
  if (disable_metadata_dynfps) {
    use_metadata_refresh_rate_ = false;
  }

  int status = HWCDisplay::Init();
  if (status) {
    return status;
  }
  color_mode_ = new HWCColorModeStc(display_intf_);
  color_mode_->Init();

  int value = 0;
  HWCDebugHandler::Get()->GetProperty(ENABLE_OPTIMIZE_REFRESH, &value);
  enable_optimize_refresh_ = (value == 1);
  if (enable_optimize_refresh_) {
    DLOGI("Drop redundant drawcycles %" PRIu64 , id_);
  }

  int vsyncs = 0;
  HWCDebugHandler::Get()->GetProperty(DEFER_FPS_FRAME_COUNT, &vsyncs);
  if (vsyncs > 0) {
    SetVsyncsApplyRateChange(UINT32(vsyncs));
  }

  is_primary_ = display_intf_->IsPrimaryDisplay();

  windowed_display_ = Debug::GetWindowRect(is_primary_, &window_rect_.left, &window_rect_.top,
                             &window_rect_.right, &window_rect_.bottom) == 0;
  DLOGI("Window rect : [%f %f %f %f] is_primary_=%d", window_rect_.left, window_rect_.top,
         window_rect_.right, window_rect_.bottom, is_primary_);

  if (is_primary_) {
    value = 0;
    HWCDebugHandler::Get()->GetProperty(ENABLE_POMS_DURING_DOZE, &value);
    enable_poms_during_doze_ = (value == 1);
    if (enable_poms_during_doze_) {
      DLOGI("Enable POMS during Doze mode %" PRIu64 , id_);
    }
  }

  HWCDebugHandler::Get()->GetProperty(PERF_HINT_WINDOW_PROP, &perf_hint_window_);
  HWCDebugHandler::Get()->GetProperty(ENABLE_PERF_HINT_LARGE_COMP_CYCLE,
                                      &perf_hint_large_comp_cycle_);

  value = 0;
  DebugHandler::Get()->GetProperty(DISABLE_DYNAMIC_FPS, &value);
  disable_dyn_fps_ = (value == 1);

  value = 0;
  DebugHandler::Get()->GetProperty(ENABLE_ROUNDED_CORNER, &value);
  enable_round_corner_ = (value == 1);

  value = 0;
  DebugHandler::Get()->GetProperty(OVERRIDE_DOZE_MODE_PROP, &value);
  override_doze_mode_ = (value == 1);
  DLOGI("override_doze_mode: %d", override_doze_mode_);

  uint32_t config_index = 0;
  GetActiveDisplayConfig(&config_index);
  DisplayConfigVariableInfo attr = {};
  GetDisplayAttributesForConfig(INT(config_index), &attr);
  active_refresh_rate_ = attr.fps;

  DLOGI("active_refresh_rate: %d", active_refresh_rate_);

  int enhance_idle_time = 0;
  HWCDebugHandler::Get()->GetProperty(ENHANCE_IDLE_TIME, &enhance_idle_time);
  enhance_idle_time_ = (enhance_idle_time == 1);
  DLOGI("enhance_idle_time: %d", enhance_idle_time);

  return status;
}

void HWCDisplayBuiltIn::Dump(std::ostringstream *os) {
  HWCDisplay::Dump(os);
  *os << histogram.Dump();
}

void HWCDisplayBuiltIn::ValidateUiScaling() {
  if (is_primary_ || !is_cmd_mode_) {
    force_reset_validate_ = false;
    return;
  }

  for (auto &hwc_layer : layer_set_) {
    Layer *layer = hwc_layer->GetSDMLayer();
    if (hwc_layer->IsScalingPresent() && !layer->input_buffer.flags.video) {
      force_reset_validate_ = true;
      return;
    }
  }
  force_reset_validate_ = false;
}

HWC2::Error HWCDisplayBuiltIn::Validate(uint32_t *out_num_types, uint32_t *out_num_requests) {
  auto status = HWC2::Error::None;
  DisplayError error = kErrorNone;

  DTRACE_SCOPED();

  // If no resources are available for the current display, mark it for GPU by pass and continue to
  // do invalidate until the resources are available
  if (display_paused_ || CheckResourceState()) {
    MarkLayersForGPUBypass();
    return status;
  }

  if (color_tranform_failed_) {
    // Must fall back to client composition
    MarkLayersForClientComposition();
  }

  // Fill in the remaining blanks in the layers and add them to the SDM layerstack
  BuildLayerStack();

  // Track damage regions if needed.
  EnablePartialUpdate();

  // Check for scaling layers during Doze mode
  ValidateUiScaling();

  // Add stitch layer to layer stack.
  AppendStitchLayer();

  // Checks and replaces layer stack for solid fill
  SolidFillPrepare();

  // Apply current Color Mode and Render Intent.
  if (color_mode_->ApplyCurrentColorModeWithRenderIntent(
      static_cast<bool>(layer_stack_.flags.hdr_present)) != HWC2::Error::None) {
    // Fallback to GPU Composition, if Color Mode can't be applied.
    MarkLayersForClientComposition();
  }

  // apply pending DE config
  PPPendingParams pending_action;
  PPDisplayAPIPayload req_payload;
  pending_action.action = kGetDetailedEnhancerData;
  pending_action.params = NULL;
  int err = display_intf_->ColorSVCRequestRoute(req_payload, NULL, &pending_action);
  if (!err && pending_action.action == kConfigureDetailedEnhancer) {
      err = SetHWDetailedEnhancerConfig(pending_action.params);
  }

  bool pending_output_dump = dump_frame_count_ && dump_output_to_file_;

  if (readback_buffer_queued_ || pending_output_dump) {
    // RHS values were set in FrameCaptureAsync() called from a binder thread. They are picked up
    // here in a subsequent draw round. Readback is not allowed for any secure use case.
    readback_configured_ = !layer_stack_.flags.secure_present;
    if (readback_configured_) {
      uint32_t cwb_with_pu_supported = 0;
      display_intf_->IsSupportedOnDisplay(kCwbCrop, &cwb_with_pu_supported);
      if (!cwb_with_pu_supported) {  // If CWB ROI isn't supported, then go for full frame update.
        DisablePartialUpdateOneFrame();
      }
      layer_stack_.output_buffer = &output_buffer_;
      layer_stack_.cwb_config = &cwb_config_;  // set the CWB config as specified by the CWB client.
      layer_stack_.flags.post_processed_output = static_cast<bool>(cwb_config_.tap_point);
    }
  }

  uint32_t num_updating_layers = GetUpdatingLayersCount();
  bool one_updating_layer = (num_updating_layers == 1);
  if (num_updating_layers != 0) {
    ToggleCPUHint(one_updating_layer);
  }

  uint32_t refresh_rate = GetOptimalRefreshRate(one_updating_layer);
  bool idle_screen = GetUpdatingAppLayersCount() == 0;
  error = display_intf_->SetRefreshRate(refresh_rate, force_refresh_rate_, idle_screen);

  // Get the refresh rate set.
  display_intf_->GetRefreshRate(&refresh_rate);
  bool vsync_source = (callbacks_->GetVsyncSource() == id_);

  if (error == kErrorNone) {
    if (vsync_source && ((current_refresh_rate_ < refresh_rate) ||
                         (enhance_idle_time_ && (current_refresh_rate_ != refresh_rate)))) {
      DTRACE_BEGIN("HWC2::Vsync::Enable");
      // Display is ramping up from idle.
      // Client realizes need for resync upon change in config.
      // Since we know config has changed, triggering vsync proactively
      // can help in reducing pipeline delays to enable events.
      SetVsyncEnabled(HWC2::Vsync::Enable);
      DTRACE_END();
    }
    // On success, set current refresh rate to new refresh rate.
    current_refresh_rate_ = refresh_rate;
  }

  if (layer_set_.empty()) {
    // Avoid flush for Command mode panel.
    flush_ = !client_connected_;
    validated_ = true;
    return status;
  }

  status = PrepareLayerStack(out_num_types, out_num_requests);
  pending_commit_ = true;
  return status;
}

HWC2::Error HWCDisplayBuiltIn::CommitLayerStack() {
  skip_commit_ = CanSkipCommit();
  return HWCDisplay::CommitLayerStack();
}

bool HWCDisplayBuiltIn::CanSkipCommit() {
  if (layer_stack_invalid_) {
    return false;
  }

  // Reject repeated drawcycle requests if it satisfies all conditions.
  // 1. None of the layerstack attributes changed.
  // 2. No new buffer latched.
  // 3. No refresh request triggered by HWC.
  // 4. This display is not source of vsync.
  bool buffers_latched = false;
  for (auto &hwc_layer : layer_set_) {
    buffers_latched |= hwc_layer->BufferLatched();
    hwc_layer->ResetBufferFlip();
  }

  bool vsync_source = (callbacks_->GetVsyncSource() == id_);
  bool skip_commit = enable_optimize_refresh_ && !pending_commit_ && !buffers_latched &&
                     !pending_refresh_ && !vsync_source;
  pending_refresh_ = false;

  return skip_commit;
}

HWC2::Error HWCDisplayBuiltIn::CommitStitchLayers() {
  if (disable_layer_stitch_) {
    return HWC2::Error::None;
  }

  if (!validated_ || skip_commit_) {
    return HWC2::Error::None;
  }

  LayerStitchContext ctx = {};
  Layer *stitch_layer = stitch_target_->GetSDMLayer();
  LayerBuffer &output_buffer = stitch_layer->input_buffer;
  for (auto &layer : layer_stack_.layers) {
    LayerComposition &composition = layer->composition;
    if (composition != kCompositionStitch) {
      continue;
    }

    StitchParams params = {};
    // Stitch target doesn't have an input fence.
    // Render all layers at specified destination.
    LayerBuffer &input_buffer = layer->input_buffer;
    params.src_hnd = reinterpret_cast<const private_handle_t *>(input_buffer.buffer_id);
    params.dst_hnd = reinterpret_cast<const private_handle_t *>(output_buffer.buffer_id);
    SetRect(layer->stitch_info.dst_rect, &params.dst_rect);
    SetRect(layer->stitch_info.slice_rect, &params.scissor_rect);
    params.src_acquire_fence = input_buffer.acquire_fence;

    ctx.stitch_params.push_back(params);
  }

  if (!ctx.stitch_params.size()) {
    // No layers marked for stitch.
    return HWC2::Error::None;
  }

  layer_stitch_task_.PerformTask(LayerStitchTaskCode::kCodeStitch, &ctx);
  // Set release fence.
  output_buffer.acquire_fence = ctx.release_fence;

  return HWC2::Error::None;
}

void HWCDisplayBuiltIn::CacheAvrStatus() {
  QSyncMode qsync_mode = kQSyncModeNone;

  DisplayError error = display_intf_->GetQSyncMode(&qsync_mode);
  if (error != kErrorNone) {
    return;
  }

  bool qsync_enabled = (qsync_mode != kQSyncModeNone);
  if (qsync_enabled_ != qsync_enabled) {
    qsync_reconfigured_ = true;
    qsync_enabled_ = qsync_enabled;
  } else {
    qsync_reconfigured_ = false;
  }
}

bool HWCDisplayBuiltIn::IsQsyncCallbackNeeded(bool *qsync_enabled, int32_t *refresh_rate,
                           int32_t *qsync_refresh_rate) {
  if (!qsync_reconfigured_) {
    return false;
  }

  bool vsync_source = (callbacks_->GetVsyncSource() == id_);
  // Qsync callback not needed if this display is not the source of vsync
  if (!vsync_source) {
    return false;
  }

  *qsync_enabled = qsync_enabled_;
  uint32_t current_rate = 0;
  display_intf_->GetRefreshRate(&current_rate);
  *refresh_rate = INT32(current_rate);
  *qsync_refresh_rate = min_refresh_rate_;

  return true;
}

void HWCDisplayBuiltIn::SetPartialUpdate(DisplayConfigFixedInfo fixed_info) {
  partial_update_enabled_ = fixed_info.partial_update || (!fixed_info.is_cmdmode);
  for (auto hwc_layer : layer_set_) {
    hwc_layer->SetPartialUpdate(partial_update_enabled_);
  }
  client_target_->SetPartialUpdate(partial_update_enabled_);
}

HWC2::Error HWCDisplayBuiltIn::SetPowerMode(HWC2::PowerMode mode, bool teardown) {
  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);
  bool command_mode = fixed_info.is_cmdmode;

  auto status = HWCDisplay::SetPowerMode(mode, teardown);
  if (status != HWC2::Error::None) {
    return status;
  }

  display_intf_->GetConfig(&fixed_info);
  is_cmd_mode_ = fixed_info.is_cmdmode;
  if (is_cmd_mode_ != command_mode) {
    SetPartialUpdate(fixed_info);
  }

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::Present(shared_ptr<Fence> *out_retire_fence) {
  auto status = HWC2::Error::None;

  DTRACE_SCOPED();

  // Proceed only if any resources are available to be allocated for the current display,
  // Otherwise keep doing invalidate
  if (CheckResourceState()) {
    Refresh();
    return status;
  }

  if (display_paused_ ) {
    return status;
  } else if (commit_state_ == kInternalCommit) {
    // Commit got triggered as part of validate.
    // Just return fence.
    *out_retire_fence = retire_fence_;
    // Client closes this fence.
    retire_fence_ = nullptr;
    // Subsequent commits have to be normal. Reset state.
    commit_state_ = kNormalCommit;
    validate_state_ = kSkipValidate;
    if (revalidate_pending_) {
      validated_ = false;
      revalidate_pending_ = false;
      if (force_reset_validate_) {
        display_intf_->ClearLUTs();
      }
    }
  } else {
    CacheAvrStatus();
    DisplayConfigFixedInfo fixed_info = {};
    display_intf_->GetConfig(&fixed_info);
    bool command_mode = fixed_info.is_cmdmode;

    status = CommitStitchLayers();
    if (status != HWC2::Error::None) {
      DLOGE("Stitch failed: %d", status);
      return status;
    }

    status = CommitLayerStack();
    if (status == HWC2::Error::None) {
      HandleFrameOutput();
      PostCommitStitchLayers();
      status = HWCDisplay::PostCommitLayerStack(out_retire_fence);
      display_intf_->GetConfig(&fixed_info);
      is_cmd_mode_ = fixed_info.is_cmdmode;
      if (is_cmd_mode_ != command_mode) {
        SetPartialUpdate(fixed_info);
      }

      // For video mode panel with dynamic fps, update the active mode index.
      // This is needed to report the correct Vsync period when client queries
      // using GetDisplayVsyncPeriod API.
      if (!is_cmd_mode_ && !disable_dyn_fps_) {
        hwc2_config_t active_config = hwc_config_map_.at(0);
        GetActiveConfig(&active_config);
        SetActiveConfigIndex(active_config);
      }
    }
  }

  pending_commit_ = false;

  // In case of scaling UI layer for command mode, reset validate
  if (force_reset_validate_) {
    if (layer_stack_.block_on_fb) {
      validated_ = false;
      display_intf_->ClearLUTs();
    } else {
      revalidate_pending_ = true;
    }
  }
  return status;
}

void HWCDisplayBuiltIn::PostCommitStitchLayers() {
  if (disable_layer_stitch_) {
    return;
  }

  // Close Stitch buffer acquire fence.
  Layer *stitch_layer = stitch_target_->GetSDMLayer();
  LayerBuffer &output_buffer = stitch_layer->input_buffer;
  for (auto &layer : layer_stack_.layers) {
    LayerComposition &composition = layer->composition;
    if (composition != kCompositionStitch) {
      continue;
    }
    LayerBuffer &input_buffer = layer->input_buffer;
    input_buffer.release_fence = output_buffer.acquire_fence;
  }
}

HWC2::Error HWCDisplayBuiltIn::GetColorModes(uint32_t *out_num_modes, ColorMode *out_modes) {
  if (out_modes == nullptr) {
    *out_num_modes = color_mode_->GetColorModeCount();
  } else {
    color_mode_->GetColorModes(out_num_modes, out_modes);
  }

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::GetRenderIntents(ColorMode mode, uint32_t *out_num_intents,
                                                RenderIntent *out_intents) {
  if (out_intents == nullptr) {
    *out_num_intents = color_mode_->GetRenderIntentCount(mode);
  } else {
    color_mode_->GetRenderIntents(mode, out_num_intents, out_intents);
  }
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetColorMode(ColorMode mode) {
  return SetColorModeWithRenderIntent(mode, RenderIntent::COLORIMETRIC);
}

HWC2::Error HWCDisplayBuiltIn::SetColorModeWithRenderIntent(ColorMode mode, RenderIntent intent) {
  auto status = color_mode_->CacheColorModeWithRenderIntent(mode, intent);
  if (status != HWC2::Error::None) {
    DLOGE("failed for mode = %d intent = %d", mode, intent);
    return status;
  }
  callbacks_->Refresh(id_);
  validated_ = false;
  return status;
}

HWC2::Error HWCDisplayBuiltIn::SetColorModeById(int32_t color_mode_id) {
  auto status = color_mode_->SetColorModeById(color_mode_id);
  if (status != HWC2::Error::None) {
    DLOGE("failed for mode = %d", color_mode_id);
    return status;
  }

  callbacks_->Refresh(id_);
  validated_ = false;

  return status;
}

HWC2::Error HWCDisplayBuiltIn::SetColorModeFromClientApi(int32_t color_mode_id) {
  DisplayError error = kErrorNone;
  std::string mode_string;

  error = display_intf_->GetColorModeName(color_mode_id, &mode_string);
  if (error) {
    DLOGE("Failed to get mode name for mode %d", color_mode_id);
    return HWC2::Error::BadParameter;
  }

  auto status = color_mode_->SetColorModeFromClientApi(mode_string);
  if (status != HWC2::Error::None) {
    DLOGE("Failed to set mode = %d", color_mode_id);
    return status;
  }

  return status;
}

HWC2::Error HWCDisplayBuiltIn::RestoreColorTransform() {
  auto status = color_mode_->RestoreColorTransform();
  if (status != HWC2::Error::None) {
    DLOGE("failed to RestoreColorTransform");
    return status;
  }

  callbacks_->Refresh(id_);

  return status;
}

HWC2::Error HWCDisplayBuiltIn::SetColorTransform(const float *matrix,
                                                 android_color_transform_t hint) {
  if (!matrix) {
    return HWC2::Error::BadParameter;
  }

  auto status = color_mode_->SetColorTransform(matrix, hint);
  if (status != HWC2::Error::None) {
    DLOGE("failed for hint = %d", hint);
    color_tranform_failed_ = true;
    return status;
  }

  callbacks_->Refresh(id_);
  color_tranform_failed_ = false;
  validated_ = false;

  return status;
}

HWC2::Error HWCDisplayBuiltIn::SetReadbackBuffer(const native_handle_t *buffer,
                                                 shared_ptr<Fence> acquire_fence,
                                                 CwbConfig cwb_config, CWBClient client) {
  if (cwb_client_ != client && cwb_client_ != kCWBClientNone) {
    DLOGE("CWB is in use with client = %d", cwb_client_);
    return HWC2::Error::NoResources;
  }

  if (secure_event_ != kSecureEventMax) {
    DLOGE("CWB is not supported as TUI transition is in progress");
    return HWC2::Error::Unsupported;
  }

  const private_handle_t *handle = reinterpret_cast<const private_handle_t *>(buffer);
  if (!handle) {
    DLOGE("Bad parameter: handle is null");
    return HWC2::Error::BadParameter;
  }

  if (handle->fd < 0) {
    DLOGE("Bad parameter: fd is null");
    return HWC2::Error::BadParameter;
  }

  if ((client == kCWBClientExternal) && ((handle->flags &
      private_handle_t::PRIV_FLAGS_UBWC_ALIGNED) || gralloc::IsUBwcFormat(handle->format))) {
    DLOGE("UBWC formats not supported for CWB");
    return HWC2::Error::Unsupported;
  }

  // Configure the output buffer as Readback buffer
  output_buffer_.width = UINT32(handle->width);
  output_buffer_.height = UINT32(handle->height);
  output_buffer_.unaligned_width = UINT32(handle->unaligned_width);
  output_buffer_.unaligned_height = UINT32(handle->unaligned_height);
  output_buffer_.format = HWCLayer::GetSDMFormat(handle->format, handle->flags);
  output_buffer_.planes[0].fd = handle->fd;
  output_buffer_.planes[0].stride = UINT32(handle->width);
  output_buffer_.acquire_fence = acquire_fence;
  output_buffer_.handle_id = handle->id;

  if (output_buffer_.format == kFormatInvalid) {
    DLOGW("Format %d is not supported by SDM", handle->format);
    return HWC2::Error::BadParameter;
  }

  readback_buffer_queued_ = true;
  readback_configured_ = false;
  validated_ = false;
  cwb_client_ = client;
  cwb_config_ = cwb_config;
  LayerRect &roi = cwb_config_.cwb_roi;
  LayerRect &full_rect = cwb_config_.cwb_full_rect;
  CwbTapPoint &tap_point = cwb_config_.tap_point;

  DisplayError error = kErrorNone;
  uint32_t buffer_width = 0, buffer_height = 0;
  error = display_intf_->GetCwbBufferResolution(tap_point, &buffer_width, &buffer_height);
  if (error) {
    DLOGE("Configuring CWB Full rect failed.");
    if (error == kErrorParameters) {
      return HWC2::Error::BadParameter;
    } else {
      return HWC2::Error::Unsupported;
    }
  } else {
    full_rect = LayerRect(0.0f, 0.0f, FLOAT(buffer_width), FLOAT(buffer_height));
  }

  DLOGV_IF(kTagClient, "CWB config from client: tap_point %d, CWB ROI Rect(%f %f %f %f), "
           "PU_as_CWB_ROI %d, Cwb full rect : (%f %f %f %f)", tap_point,
           roi.left, roi.top, roi.right, roi.bottom, cwb_config_.pu_as_cwb_roi,
           full_rect.left, full_rect.top, full_rect.right, full_rect.bottom);

  DLOGV_IF(kTagClient, "Successfully configured the output buffer: readback_buffer_queued_ %d, "
           "readback_configured_ %d, cwb_client_ %d", readback_buffer_queued_,
           readback_configured_, cwb_client_);

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::GetReadbackBufferFence(shared_ptr<Fence> *release_fence) {
  auto status = HWC2::Error::None;

  if (readback_configured_ && output_buffer_.release_fence) {
    *release_fence = output_buffer_.release_fence;
    DLOGI("Successfully retrieved readback buffer fence for cwb clinet %d on tappoint %d",
          cwb_client_, cwb_config_.tap_point);
  } else {
    DLOGE("Failed to retrieve readback buffer fence: readback_configured_ %d, "
          "output_buffer_.release_fence for client %d on tappoint %d ",
          readback_configured_, cwb_client_, cwb_config_.tap_point);
    status = HWC2::Error::Unsupported;
  }

  cwb_config_ = {};
  readback_buffer_queued_ = false;
  readback_configured_ = false;
  output_buffer_ = {};
  cwb_client_ = kCWBClientNone;

  DLOGV_IF(kTagQDCM, "Successfully retrieved the buffer: cwb_tap_point %d, "
           "readback_buffer_queued_ %d, readback_configured_ %d",
           cwb_config_.tap_point, readback_buffer_queued_, readback_configured_);

  return status;
}

DisplayError HWCDisplayBuiltIn::TeardownConcurrentWriteback(bool *needs_refresh) {
  if (!needs_refresh) {
    return kErrorParameters;
  }
  if (cwb_client_ == kCWBClientNone) {
    *needs_refresh = false;
    return kErrorNone;
  }
  if (cwb_client_ == kCWBClientFrameDump) {
    dump_frame_count_ = 0;
    dump_output_to_file_ = false;
    // Unmap and Free buffer
    if (munmap(output_buffer_base_, output_buffer_info_.alloc_buffer_info.size) != 0) {
      DLOGW("unmap failed with err %d", errno);
    }
    if (buffer_allocator_->FreeBuffer(&output_buffer_info_) != 0) {
      DLOGW("FreeBuffer failed");
    }
    output_buffer_info_ = {};
    output_buffer_base_ = nullptr;
  } else if (cwb_client_ == kCWBClientColor) {
    frame_capture_buffer_queued_ = false;
    frame_capture_status_ = 0;
  }
  readback_buffer_queued_ = false;
  cwb_config_ = {};
  readback_configured_ = false;
  output_buffer_ = {};
  cwb_client_ = kCWBClientNone;
  validated_ = false;

  *needs_refresh = true;
  return kErrorNone;
}

DisplayError HWCDisplayBuiltIn::TeardownCwbForVirtualDisplay() {
  DisplayError error = kErrorNotSupported;
  if (Fence::Wait(output_buffer_.release_fence) != kErrorNone) {
    DLOGE("sync_wait error errno = %d, desc = %s", errno, strerror(errno));
    return kErrorResources;
  }
  if (display_intf_) {
    error = display_intf_->TeardownConcurrentWriteback();
  }

  readback_buffer_queued_ = false;
  cwb_config_ = {};
  readback_configured_ = false;
  output_buffer_ = {};
  cwb_client_ = kCWBClientNone;

  return error;
}

HWC2::Error HWCDisplayBuiltIn::SetDisplayDppsAdROI(uint32_t h_start, uint32_t h_end,
                                                   uint32_t v_start, uint32_t v_end,
                                                   uint32_t factor_in, uint32_t factor_out) {
  DisplayError error = kErrorNone;
  DisplayDppsAd4RoiCfg dpps_ad4_roi_cfg = {};
  uint32_t panel_width = 0, panel_height = 0;
  constexpr uint16_t kMaxFactorVal = 0xffff;

  if (h_start >= h_end || v_start >= v_end || factor_in > kMaxFactorVal ||
      factor_out > kMaxFactorVal) {
    DLOGE("Invalid roi region = [%u, %u, %u, %u, %u, %u]",
           h_start, h_end, v_start, v_end, factor_in, factor_out);
    return HWC2::Error::BadParameter;
  }

  GetPanelResolution(&panel_width, &panel_height);

  if (h_start >= panel_width || h_end > panel_width ||
      v_start >= panel_height || v_end > panel_height) {
    DLOGE("Invalid roi region = [%u, %u, %u, %u], panel resolution = [%u, %u]",
           h_start, h_end, v_start, v_end, panel_width, panel_height);
    return HWC2::Error::BadParameter;
  }

  dpps_ad4_roi_cfg.h_start = h_start;
  dpps_ad4_roi_cfg.h_end = h_end;
  dpps_ad4_roi_cfg.v_start = v_start;
  dpps_ad4_roi_cfg.v_end = v_end;
  dpps_ad4_roi_cfg.factor_in = factor_in;
  dpps_ad4_roi_cfg.factor_out = factor_out;

  error = display_intf_->SetDisplayDppsAdROI(&dpps_ad4_roi_cfg);
  if (error)
    return HWC2::Error::BadConfig;

  callbacks_->Refresh(id_);

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetFrameTriggerMode(uint32_t mode) {
  DisplayError error = kErrorNone;
  FrameTriggerMode trigger_mode = kFrameTriggerDefault;

  if (mode >= kFrameTriggerMax) {
    DLOGE("Invalid input mode %d", mode);
    return HWC2::Error::BadParameter;
  }

  trigger_mode = static_cast<FrameTriggerMode>(mode);
  error = display_intf_->SetFrameTriggerMode(trigger_mode);
  if (error)
    return HWC2::Error::BadConfig;

  callbacks_->Refresh(HWC_DISPLAY_PRIMARY);
  validated_ = false;

  return HWC2::Error::None;
}

int HWCDisplayBuiltIn::Perform(uint32_t operation, ...) {
  va_list args;
  va_start(args, operation);
  int val = 0;
  LayerSolidFill *solid_fill_color;
  LayerRect *rect = NULL;

  switch (operation) {
    case SET_METADATA_DYN_REFRESH_RATE:
      val = va_arg(args, int32_t);
      SetMetaDataRefreshRateFlag(val);
      break;
    case SET_BINDER_DYN_REFRESH_RATE:
      val = va_arg(args, int32_t);
      ForceRefreshRate(UINT32(val));
      break;
    case SET_DISPLAY_MODE:
      val = va_arg(args, int32_t);
      SetDisplayMode(UINT32(val));
      break;
    case SET_QDCM_SOLID_FILL_INFO:
      solid_fill_color = va_arg(args, LayerSolidFill*);
      SetQDCMSolidFillInfo(true, *solid_fill_color);
      break;
    case UNSET_QDCM_SOLID_FILL_INFO:
      solid_fill_color = va_arg(args, LayerSolidFill*);
      SetQDCMSolidFillInfo(false, *solid_fill_color);
      break;
    case SET_QDCM_SOLID_FILL_RECT:
      rect = va_arg(args, LayerRect*);
      solid_fill_rect_ = *rect;
      break;
    default:
      DLOGW("Invalid operation %d", operation);
      va_end(args);
      return -EINVAL;
  }
  va_end(args);
  validated_ = false;

  return 0;
}

DisplayError HWCDisplayBuiltIn::SetDisplayMode(uint32_t mode) {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->SetDisplayMode(mode);
    if (error == kErrorNone) {
      DisplayConfigFixedInfo fixed_info = {};
      display_intf_->GetConfig(&fixed_info);
      is_cmd_mode_ = fixed_info.is_cmdmode;
      partial_update_enabled_ = fixed_info.partial_update;
      for (auto hwc_layer : layer_set_) {
        hwc_layer->SetPartialUpdate(partial_update_enabled_);
      }
      client_target_->SetPartialUpdate(partial_update_enabled_);
    }
  }

  return error;
}

void HWCDisplayBuiltIn::SetMetaDataRefreshRateFlag(bool enable) {
  int disable_metadata_dynfps = 0;

  HWCDebugHandler::Get()->GetProperty(DISABLE_METADATA_DYNAMIC_FPS_PROP, &disable_metadata_dynfps);
  if (disable_metadata_dynfps) {
    return;
  }
  use_metadata_refresh_rate_ = enable;
}

void HWCDisplayBuiltIn::SetQDCMSolidFillInfo(bool enable, const LayerSolidFill &color) {
  solid_fill_enable_ = enable;
  solid_fill_color_ = color;
}

void HWCDisplayBuiltIn::ToggleCPUHint(bool set) {
  if (!cpu_hint_ || !perf_hint_window_) {
    return;
  }

  if (set) {
    cpu_hint_->Set();
  } else {
    cpu_hint_->Reset();
  }
}

int HWCDisplayBuiltIn::GetActiveSecureSession(std::bitset<kSecureMax> *secure_sessions) {
  if (!secure_sessions) {
    return -1;
  }
  secure_sessions->reset();
  for (auto hwc_layer : layer_set_) {
    Layer *layer = hwc_layer->GetSDMLayer();
    if (layer->input_buffer.flags.secure_camera) {
      secure_sessions->set(kSecureCamera);
    }
    if (layer->input_buffer.flags.secure_display) {
      secure_sessions->set(kSecureDisplay);
    }
  }
  if (secure_event_ == kTUITransitionStart || secure_event_ == kTUITransitionPrepare) {
    secure_sessions->set(kSecureTUI);
  }
  return 0;
}

int HWCDisplayBuiltIn::HandleSecureSession(const std::bitset<kSecureMax> &secure_sessions,
                                           bool *power_on_pending, bool is_active_secure_display) {
  if (!power_on_pending) {
    return -EINVAL;
  }

  if (!is_active_secure_display) {
    // Do handling as done on non-primary displays.
    DLOGI("Default handling for display %" PRIu64 " %d-%d", id_, sdm_id_, type_);
    return HWCDisplay::HandleSecureSession(secure_sessions, power_on_pending,
                                           is_active_secure_display);
  }

  if (current_power_mode_ != HWC2::PowerMode::On) {
    return 0;
  }

  if (active_secure_sessions_[kSecureDisplay] != secure_sessions[kSecureDisplay]) {
    SecureEvent secure_event =
        secure_sessions.test(kSecureDisplay) ? kSecureDisplayStart : kSecureDisplayEnd;
    bool needs_refresh = false;
    DisplayError err = display_intf_->HandleSecureEvent(secure_event, &needs_refresh);
    if (err != kErrorNone) {
      DLOGE("Set secure event failed");
      return err;
    }

    DLOGI("SecureDisplay state changed from %d to %d for display %" PRIu64 " %d-%d",
          active_secure_sessions_.test(kSecureDisplay), secure_sessions.test(kSecureDisplay),
          id_, sdm_id_, type_);
  }
  active_secure_sessions_ = secure_sessions;
  *power_on_pending = false;
  return 0;
}

void HWCDisplayBuiltIn::ForceRefreshRate(uint32_t refresh_rate) {
  if ((refresh_rate && (refresh_rate < min_refresh_rate_ || refresh_rate > max_refresh_rate_)) ||
      force_refresh_rate_ == refresh_rate) {
    // Cannot honor force refresh rate, as its beyond the range or new request is same
    return;
  }

  force_refresh_rate_ = refresh_rate;

  callbacks_->Refresh(id_);

  return;
}

uint32_t HWCDisplayBuiltIn::GetOptimalRefreshRate(bool one_updating_layer) {
  if (force_refresh_rate_) {
    return force_refresh_rate_;
  } else if (use_metadata_refresh_rate_ && one_updating_layer && metadata_refresh_rate_) {
    return metadata_refresh_rate_;
  }

  DLOGV_IF(kTagClient, "active_refresh_rate_: %d", active_refresh_rate_);
  return active_refresh_rate_;
}

void HWCDisplayBuiltIn::SetIdleTimeoutMs(uint32_t timeout_ms, uint32_t inactive_ms) {
  display_intf_->SetIdleTimeoutMs(timeout_ms, inactive_ms);
  validated_ = false;
}

void HWCDisplayBuiltIn::HandleFrameOutput() {
  if (readback_buffer_queued_) {
    DLOGV_IF(kTagQDCM, "No pending readback buffer found on the queue.");
    validated_ = false;
  }

  if (frame_capture_buffer_queued_) {
    DLOGV_IF(kTagQDCM, "frame_capture_buffer_queued_ is in use. Handle frame capture.");
    HandleFrameCapture();
  } else if (dump_output_to_file_) {
    DLOGV_IF(kTagQDCM, "dump_output_to_file is in use. Handle frame dump.");
    HandleFrameDump();
  }
}

void HWCDisplayBuiltIn::HandleFrameCapture() {
  if (readback_configured_ && output_buffer_.release_fence) {
    frame_capture_status_ = Fence::Wait(output_buffer_.release_fence);
  }

  DLOGV_IF(kTagQDCM, "Frame captured successfully for cwb_client %d on cwb_tap_point %d",
           cwb_client_, cwb_config_.tap_point);

  frame_capture_buffer_queued_ = false;
  readback_buffer_queued_ = false;
  cwb_config_ = {};
  readback_configured_ = false;
  output_buffer_ = {};
  cwb_client_ = kCWBClientNone;

  DLOGV_IF(kTagQDCM, "Frame captured: frame_capture_buffer_queued_ %d "
           "readback_buffer_queued_ %d cwb_tap_point %d readback_configured_ %d "
           "cwb_client_ %d", frame_capture_buffer_queued_, readback_buffer_queued_,
           cwb_config_.tap_point, readback_configured_, cwb_client_);
}

void HWCDisplayBuiltIn::HandleFrameDump() {
  if (dump_frame_count_) {
    int ret = 0;
    ret = Fence::Wait(output_buffer_.release_fence);
    if (ret != kErrorNone) {
      DLOGE("sync_wait error errno = %d, desc = %s", errno, strerror(errno));
    }

    if (!ret) {
      DumpOutputBuffer(output_buffer_info_, output_buffer_base_, layer_stack_.retire_fence);
      validated_ = false;
    }

    if (0 == (dump_frame_count_ - 1)) {
      dump_output_to_file_ = false;
      // Unmap and Free buffer
      if (munmap(output_buffer_base_, output_buffer_info_.alloc_buffer_info.size) != 0) {
        DLOGE("unmap failed with err %d", errno);
      }
      if (buffer_allocator_->FreeBuffer(&output_buffer_info_) != 0) {
        DLOGE("FreeBuffer failed");
      }

      readback_buffer_queued_ = false;
      cwb_config_ = {};
      readback_configured_ = false;

      output_buffer_ = {};
      output_buffer_info_ = {};
      output_buffer_base_ = nullptr;
      cwb_client_ = kCWBClientNone;
    }
  }
}

HWC2::Error HWCDisplayBuiltIn::SetFrameDumpConfig(uint32_t count, uint32_t bit_mask_layer_type,
                                                  int32_t format, const CwbConfig &cwb_config) {
  bool dump_output_to_file = bit_mask_layer_type & (1 << OUTPUT_LAYER_DUMP);
  DLOGI("output_layer_dump_enable %d", dump_output_to_file_);

  if (dump_output_to_file) {
    if (cwb_client_ != kCWBClientNone) {
      DLOGW("CWB is in use with client = %d", cwb_client_);
      return HWC2::Error::NoResources;
    }
  }

  if (!count || (dump_output_to_file && (output_buffer_info_.alloc_buffer_info.fd >= 0))) {
    DLOGW("FrameDump Not enabled Framecount = %d dump_output_to_file = %d o/p fd = %d", count,
          dump_output_to_file, output_buffer_info_.alloc_buffer_info.fd);
    return HWC2::Error::None;
  }

  HWCDisplay::SetFrameDumpConfig(count, bit_mask_layer_type, format, cwb_config);

  if (!dump_output_to_file) {
    // output(cwb) not requested, return
    return HWC2::Error::None;
  }

  // Allocate and map output buffer
  const CwbTapPoint &tap_point = cwb_config.tap_point;
  if (GetCwbBufferResolution(tap_point, &output_buffer_info_.buffer_config.width,
                             &output_buffer_info_.buffer_config.height)) {
    DLOGW("Buffer Resolution setting failed.");
    return HWC2::Error::BadConfig;
  }

  DLOGV_IF(kTagQDCM, "CWB output buffer resolution: width:%d height:%d tap point:%s",
           output_buffer_info_.buffer_config.width, output_buffer_info_.buffer_config.height,
           UINT32(tap_point) ? "DSPP" : "LM");

  output_buffer_info_.buffer_config.format = HWCLayer::GetSDMFormat(format, 0);
  output_buffer_info_.buffer_config.buffer_count = 1;
  if (buffer_allocator_->AllocateBuffer(&output_buffer_info_) != 0) {
    DLOGE("Buffer allocation failed");
    output_buffer_info_ = {};
    return HWC2::Error::NoResources;
  }

  void *buffer = mmap(NULL, output_buffer_info_.alloc_buffer_info.size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, output_buffer_info_.alloc_buffer_info.fd, 0);

  if (buffer == MAP_FAILED) {
    DLOGE("mmap failed with err %d", errno);
    buffer_allocator_->FreeBuffer(&output_buffer_info_);
    output_buffer_info_ = {};
    return HWC2::Error::NoResources;
  }

  output_buffer_base_ = buffer;
  const native_handle_t *handle = static_cast<native_handle_t *>(output_buffer_info_.private_data);
  HWC2::Error err = SetReadbackBuffer(handle, nullptr, cwb_config, kCWBClientFrameDump);
  if (err != HWC2::Error::None) {
    return err;
  }
  dump_output_to_file_ = dump_output_to_file;

  return HWC2::Error::None;
}

int HWCDisplayBuiltIn::ValidateFrameCaptureConfig(const BufferInfo &output_buffer_info,
                                                  const CwbTapPoint &cwb_tappoint) {
  if (cwb_tappoint < CwbTapPoint::kLmTapPoint || cwb_tappoint > CwbTapPoint::kDsppTapPoint) {
    DLOGE("Invalid CWB tappoint passed by client ");
    return -1;
  } else if (cwb_tappoint == CwbTapPoint::kDsppTapPoint) {
    auto panel_width = 0u;
    auto panel_height = 0u;
    GetPanelResolution(&panel_width, &panel_height);
    if (output_buffer_info.buffer_config.width < panel_width ||
        output_buffer_info.buffer_config.height < panel_height) {
      DLOGE("Buffer dimensions should not be less than panel resolution");
      return -1;
    }
  } else if (cwb_tappoint == CwbTapPoint::kLmTapPoint) {
    uint32_t dest_scalar_enabled = 0;
    display_intf_->IsSupportedOnDisplay(kDestinationScalar, &dest_scalar_enabled);
    if (dest_scalar_enabled) {
      auto mixer_width = 0u;
      auto mixer_height = 0u;
      GetMixerResolution(&mixer_width, &mixer_height);
      if (output_buffer_info.buffer_config.width < mixer_width ||
          output_buffer_info.buffer_config.height < mixer_height) {
        DLOGE("Buffer dimensions should not be less than LM resolution");
        return -1;
      }
    } else {
      auto fb_width = 0u;
      auto fb_height = 0u;
      GetFrameBufferResolution(&fb_width, &fb_height);
      if (output_buffer_info.buffer_config.width < fb_width ||
          output_buffer_info.buffer_config.height < fb_height) {
        DLOGE("Buffer dimensions should not be less than FB resolution");
        return -1;
      }
    }
  }
  return 0;
}

int HWCDisplayBuiltIn::FrameCaptureAsync(const BufferInfo &output_buffer_info,
                                         const CwbConfig &cwb_config) {
  if (cwb_client_ != kCWBClientNone) {
    DLOGE("CWB is in use with client = %d", cwb_client_);
    return -1;
  }

  // Note: This function is called in context of a binder thread and a lock is already held
  if (output_buffer_info.alloc_buffer_info.fd < 0) {
    DLOGE("Invalid fd %d", output_buffer_info.alloc_buffer_info.fd);
    return -1;
  }

  int error = ValidateFrameCaptureConfig(output_buffer_info, cwb_config.tap_point);
  if (error) {
    return -1;
  }

  const native_handle_t *buffer = static_cast<native_handle_t *>(output_buffer_info.private_data);
  SetReadbackBuffer(buffer, nullptr, cwb_config, kCWBClientColor);
  frame_capture_buffer_queued_ = true;
  frame_capture_status_ = -EAGAIN;

  return 0;
}

DisplayError HWCDisplayBuiltIn::SetDetailEnhancerConfig
                                   (const DisplayDetailEnhancerData &de_data) {
  DisplayError error = kErrorNotSupported;

  if (display_intf_) {
    error = display_intf_->SetDetailEnhancerData(de_data);
    validated_ = false;
  }
  return error;
}

DisplayError HWCDisplayBuiltIn::SetHWDetailedEnhancerConfig(void *params) {
  DisplayError err = kErrorNone;
  DisplayDetailEnhancerData de_data;

  PPDETuningCfgData *de_tuning_cfg_data = reinterpret_cast<PPDETuningCfgData*>(params);
  if (de_tuning_cfg_data->cfg_pending) {
    if (!de_tuning_cfg_data->cfg_en) {
      de_data.enable = 0;
      DLOGV_IF(kTagQDCM, "Disable DE config");
    } else {
      de_data.override_flags = kOverrideDEEnable;
      de_data.enable = 1;

      DLOGV_IF(kTagQDCM, "Enable DE: flags %u, sharp_factor %d, thr_quiet %d, thr_dieout %d, "
        "thr_low %d, thr_high %d, clip %d, quality %d, content_type %d, de_blend %d",
        de_tuning_cfg_data->params.flags, de_tuning_cfg_data->params.sharp_factor,
        de_tuning_cfg_data->params.thr_quiet, de_tuning_cfg_data->params.thr_dieout,
        de_tuning_cfg_data->params.thr_low, de_tuning_cfg_data->params.thr_high,
        de_tuning_cfg_data->params.clip, de_tuning_cfg_data->params.quality,
        de_tuning_cfg_data->params.content_type, de_tuning_cfg_data->params.de_blend);

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagSharpFactor) {
        de_data.override_flags |= kOverrideDESharpen1;
        de_data.sharp_factor = de_tuning_cfg_data->params.sharp_factor;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagClip) {
        de_data.override_flags |= kOverrideDEClip;
        de_data.clip = de_tuning_cfg_data->params.clip;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrQuiet) {
        de_data.override_flags |= kOverrideDEThrQuiet;
        de_data.thr_quiet = de_tuning_cfg_data->params.thr_quiet;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrDieout) {
        de_data.override_flags |= kOverrideDEThrDieout;
        de_data.thr_dieout = de_tuning_cfg_data->params.thr_dieout;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrLow) {
        de_data.override_flags |= kOverrideDEThrLow;
        de_data.thr_low = de_tuning_cfg_data->params.thr_low;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrHigh) {
        de_data.override_flags |= kOverrideDEThrHigh;
        de_data.thr_high = de_tuning_cfg_data->params.thr_high;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagContentQualLevel) {
        switch (de_tuning_cfg_data->params.quality) {
          case kDeContentQualLow:
            de_data.quality_level = kContentQualityLow;
            break;
          case kDeContentQualMedium:
            de_data.quality_level = kContentQualityMedium;
            break;
          case kDeContentQualHigh:
            de_data.quality_level = kContentQualityHigh;
            break;
          case kDeContentQualUnknown:
          default:
            de_data.quality_level = kContentQualityUnknown;
            break;
        }
      }

      switch (de_tuning_cfg_data->params.content_type) {
        case kDeContentTypeVideo:
          de_data.content_type = kContentTypeVideo;
          break;
        case kDeContentTypeGraphics:
          de_data.content_type = kContentTypeGraphics;
          break;
        case kDeContentTypeUnknown:
        default:
          de_data.content_type = kContentTypeUnknown;
          break;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagDeBlend) {
        de_data.override_flags |= kOverrideDEBlend;
        de_data.de_blend = de_tuning_cfg_data->params.de_blend;
      }
    }
    err = SetDetailEnhancerConfig(de_data);
    if (err) {
      DLOGW("SetDetailEnhancerConfig failed. err = %d", err);
    }
    de_tuning_cfg_data->cfg_pending = false;
  }
  return err;
}

DisplayError HWCDisplayBuiltIn::ControlPartialUpdate(bool enable, uint32_t *pending) {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->ControlPartialUpdate(enable, pending);
    validated_ = false;
  }

  return error;
}

DisplayError HWCDisplayBuiltIn::DisablePartialUpdateOneFrame() {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->DisablePartialUpdateOneFrame();
    validated_ = false;
  }

  return error;
}

HWC2::Error HWCDisplayBuiltIn::SetDisplayedContentSamplingEnabledVndService(bool enabled) {
  std::unique_lock<decltype(sampling_mutex)> lk(sampling_mutex);
  vndservice_sampling_vote = enabled;
  if (api_sampling_vote || vndservice_sampling_vote) {
    histogram.start();
    display_intf_->colorSamplingOn();
  } else {
    display_intf_->colorSamplingOff();
    histogram.stop();
  }
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetDisplayedContentSamplingEnabled(int32_t enabled,
                                                                  uint8_t component_mask,
                                                                  uint64_t max_frames) {
  if ((enabled != HWC2_DISPLAYED_CONTENT_SAMPLING_ENABLE) &&
      (enabled != HWC2_DISPLAYED_CONTENT_SAMPLING_DISABLE))
    return HWC2::Error::BadParameter;

  std::unique_lock<decltype(sampling_mutex)> lk(sampling_mutex);
  if (enabled == HWC2_DISPLAYED_CONTENT_SAMPLING_ENABLE) {
    api_sampling_vote = true;
  } else {
    api_sampling_vote = false;
  }

  auto start = api_sampling_vote || vndservice_sampling_vote;
  if (start && max_frames == 0) {
    histogram.start();
    display_intf_->colorSamplingOn();
  } else if (start) {
    histogram.start(max_frames);
    display_intf_->colorSamplingOn();
  } else {
    display_intf_->colorSamplingOff();
    histogram.stop();
  }
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::GetDisplayedContentSamplingAttributes(
    int32_t *format, int32_t *dataspace, uint8_t *supported_components) {
  return histogram.getAttributes(format, dataspace, supported_components);
}

HWC2::Error HWCDisplayBuiltIn::GetDisplayedContentSample(
    uint64_t max_frames, uint64_t timestamp, uint64_t *numFrames,
    int32_t samples_size[NUM_HISTOGRAM_COLOR_COMPONENTS],
    uint64_t *samples[NUM_HISTOGRAM_COLOR_COMPONENTS]) {
  histogram.collect(max_frames, timestamp, samples_size, samples, numFrames);
  return HWC2::Error::None;
}

DisplayError HWCDisplayBuiltIn::SetMixerResolution(uint32_t width, uint32_t height) {
  DisplayError error = display_intf_->SetMixerResolution(width, height);
  callbacks_->Refresh(id_);
  validated_ = false;
  return error;
}

DisplayError HWCDisplayBuiltIn::GetMixerResolution(uint32_t *width, uint32_t *height) {
  return display_intf_->GetMixerResolution(width, height);
}

HWC2::Error HWCDisplayBuiltIn::SetQSyncMode(QSyncMode qsync_mode) {
  // Client needs to ensure that config change and qsync mode change
  // are not triggered in the same drawcycle.
  if (pending_config_) {
    DLOGE("Failed to set qsync mode. Pending active config transition");
    return HWC2::Error::Unsupported;
  }

  auto err = display_intf_->SetQSyncMode(qsync_mode);
  if (err != kErrorNone) {
    return HWC2::Error::Unsupported;
  }

  validated_ = false;
  return HWC2::Error::None;
}

DisplayError HWCDisplayBuiltIn::ControlIdlePowerCollapse(bool enable, bool synchronous) {
  DisplayError error = kErrorNone;

  if (display_intf_) {
    error = display_intf_->ControlIdlePowerCollapse(enable, synchronous);
    validated_ = false;
  }
  return error;
}

DisplayError HWCDisplayBuiltIn::SetDynamicDSIClock(uint64_t bitclk) {
  DisablePartialUpdateOneFrame();
  DisplayError error = display_intf_->SetDynamicDSIClock(bitclk);
  if (error != kErrorNone) {
    DLOGE(" failed: Clk: %" PRIu64 " Error: %d", bitclk, error);
    return error;
  }

  callbacks_->Refresh(id_);
  validated_ = false;

  return kErrorNone;
}

DisplayError HWCDisplayBuiltIn::GetDynamicDSIClock(uint64_t *bitclk) {
  if (display_intf_) {
    return display_intf_->GetDynamicDSIClock(bitclk);
  }

  return kErrorNotSupported;
}

DisplayError HWCDisplayBuiltIn::GetSupportedDSIClock(std::vector<uint64_t> *bitclk_rates) {
  if (display_intf_) {
    return display_intf_->GetSupportedDSIClock(bitclk_rates);
  }

  return kErrorNotSupported;
}

HWC2::Error HWCDisplayBuiltIn::UpdateDisplayId(hwc2_display_t id) {
  id_ = id;
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetPendingRefresh() {
  pending_refresh_ = true;
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetPanelBrightness(float brightness) {
  DisplayError ret = display_intf_->SetPanelBrightness(brightness);
  if (ret != kErrorNone) {
    return HWC2::Error::NoResources;
  }

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::GetPanelBrightness(float *brightness) {
  DisplayError ret = display_intf_->GetPanelBrightness(brightness);
  if (ret != kErrorNone) {
    return HWC2::Error::NoResources;
  }

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::GetPanelMaxBrightness(uint32_t *max_brightness_level) {
  DisplayError ret = display_intf_->GetPanelMaxBrightness(max_brightness_level);
  if (ret != kErrorNone) {
    return HWC2::Error::NoResources;
  }

  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetBLScale(uint32_t level) {
  DisplayError ret = display_intf_->SetBLScale(level);
  if (ret != kErrorNone) {
    return HWC2::Error::NoResources;
  }
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::UpdatePowerMode(HWC2::PowerMode mode) {
  current_power_mode_ = mode;
  validated_ = false;
  return HWC2::Error::None;
}

HWC2::Error HWCDisplayBuiltIn::SetClientTarget(buffer_handle_t target,
                                               shared_ptr<Fence> acquire_fence,
                                               int32_t dataspace, hwc_region_t damage) {
  HWC2::Error error = HWCDisplay::SetClientTarget(target, acquire_fence, dataspace, damage);
  if (error != HWC2::Error::None) {
    return error;
  }

  // windowed_display and dynamic scaling are not supported.
  if (windowed_display_) {
    return HWC2::Error::None;
  }

  Layer *sdm_layer = client_target_->GetSDMLayer();
  uint32_t fb_width = 0, fb_height = 0;

  GetFrameBufferResolution(&fb_width, &fb_height);

  if (fb_width != sdm_layer->input_buffer.unaligned_width ||
      fb_height != sdm_layer->input_buffer.unaligned_height) {
    if (SetFrameBufferConfig(sdm_layer->input_buffer.unaligned_width,
                             sdm_layer->input_buffer.unaligned_height)) {
      return HWC2::Error::BadParameter;
    }
  }

  return HWC2::Error::None;
}

bool HWCDisplayBuiltIn::IsSmartPanelConfig(uint32_t config_id) {
  if (config_id < hwc_config_map_.size()) {
    uint32_t index = hwc_config_map_.at(config_id);
    return variable_config_map_.at(index).smart_panel;
  }

  return false;
}

bool HWCDisplayBuiltIn::HasSmartPanelConfig(void) {
  if (!enable_poms_during_doze_) {
    uint32_t config = 0;
    GetActiveDisplayConfig(&config);
    return IsSmartPanelConfig(config);
  }

  for (auto &config : variable_config_map_) {
    if (config.second.smart_panel) {
      return true;
    }
  }

  return false;
}

int HWCDisplayBuiltIn::Deinit() {
  // Destory color convert instance. This destroys thread and underlying GL resources.
  if (gl_layer_stitch_) {
    layer_stitch_task_.PerformTask(LayerStitchTaskCode::kCodeDestroyInstance, nullptr);
  }

  histogram.stop();
  return HWCDisplay::Deinit();
}

void HWCDisplayBuiltIn::OnTask(const LayerStitchTaskCode &task_code,
                               SyncTask<LayerStitchTaskCode>::TaskContext *task_context) {
  switch (task_code) {
    case LayerStitchTaskCode::kCodeGetInstance: {
        gl_layer_stitch_ = GLLayerStitch::GetInstance(false /* Non-secure */);
      }
      break;
    case LayerStitchTaskCode::kCodeStitch: {
        DTRACE_SCOPED();
        LayerStitchContext* ctx = reinterpret_cast<LayerStitchContext*>(task_context);
        gl_layer_stitch_->Blit(ctx->stitch_params, &(ctx->release_fence));
      }
      break;
    case LayerStitchTaskCode::kCodeDestroyInstance: {
        if (gl_layer_stitch_) {
          GLLayerStitch::Destroy(gl_layer_stitch_);
        }
      }
      break;
  }
}

bool HWCDisplayBuiltIn::InitLayerStitch() {
  if (!is_primary_) {
    // Disable on all non-primary builtins.
    DLOGI("Non-primary builtin.");
    disable_layer_stitch_ = true;
    return true;
  }

  // Disable by default.
  int value = 1;
  Debug::Get()->GetProperty(DISABLE_LAYER_STITCH, &value);
  disable_layer_stitch_ = (value == 1);

  if (disable_layer_stitch_) {
    DLOGI("Layer Stitch Disabled !!!");
    return true;
  }

  // Initialize stitch context. This will be non-secure.
  layer_stitch_task_.PerformTask(LayerStitchTaskCode::kCodeGetInstance, nullptr);
  if (gl_layer_stitch_ == nullptr) {
    DLOGE("Failed to get LayerStitch Instance");
    return false;
  }

  if (!AllocateStitchBuffer()) {
    return true;
  }

  stitch_target_ = new HWCLayer(id_, static_cast<HWCBufferAllocator *>(buffer_allocator_));

  // Populate buffer params and pvt handle.
  InitStitchTarget();

  DLOGI("Created LayerStitch instance: %p", gl_layer_stitch_);

  return true;
}

bool HWCDisplayBuiltIn::AllocateStitchBuffer() {
  // Buffer dimensions: FB width * (1.5 * height)

  DisplayError error = display_intf_->GetFrameBufferConfig(&fb_config_);
  if (error != kErrorNone) {
    DLOGE("Get frame buffer config failed. Error = %d", error);
    return false;
  }

  BufferConfig &config = buffer_info_.buffer_config;
  config.width = fb_config_.x_pixels;
  config.height = fb_config_.y_pixels * kBufferHeightFactor;

  // By default UBWC is enabled and below property is global enable/disable for all
  // buffers allocated through gralloc , including framebuffer targets.
  int ubwc_disabled = 0;
  HWCDebugHandler::Get()->GetProperty(DISABLE_UBWC_PROP, &ubwc_disabled);
  config.format = ubwc_disabled ? kFormatRGBA8888 : kFormatRGBA8888Ubwc;

  config.gfx_client = true;

  // Populate default params.
  config.secure = false;
  config.cache = false;
  config.secure_camera = false;

  int err = buffer_allocator_->AllocateBuffer(&buffer_info_);

  if (err != 0) {
    DLOGE("Failed to allocate buffer. Error: %d", error);
    return false;
  }

  return true;
}

void HWCDisplayBuiltIn::InitStitchTarget() {
  LayerBuffer buffer = {};
  buffer.planes[0].fd = buffer_info_.alloc_buffer_info.fd;
  buffer.planes[0].offset = 0;
  buffer.planes[0].stride = buffer_info_.alloc_buffer_info.stride;
  buffer.size = buffer_info_.alloc_buffer_info.size;
  buffer.handle_id = buffer_info_.alloc_buffer_info.id;
  buffer.width = buffer_info_.alloc_buffer_info.aligned_width;
  buffer.height = buffer_info_.alloc_buffer_info.aligned_height;
  buffer.unaligned_width = fb_config_.x_pixels;
  buffer.unaligned_height = fb_config_.y_pixels * kBufferHeightFactor;
  buffer.format = buffer_info_.alloc_buffer_info.format;

  Layer *sdm_stitch_target = stitch_target_->GetSDMLayer();
  sdm_stitch_target->composition = kCompositionStitchTarget;
  sdm_stitch_target->input_buffer = buffer;
  sdm_stitch_target->input_buffer.buffer_id = reinterpret_cast<uint64_t>(buffer_info_.private_data);
}

void HWCDisplayBuiltIn::AppendStitchLayer() {
  if (disable_layer_stitch_) {
    return;
  }

  // Append stitch target buffer to layer stack.
  Layer *sdm_stitch_target = stitch_target_->GetSDMLayer();
  sdm_stitch_target->composition = kCompositionStitchTarget;
  sdm_stitch_target->dst_rect = {0, 0, FLOAT(fb_config_.x_pixels), FLOAT(fb_config_.y_pixels)};
  layer_stack_.layers.push_back(sdm_stitch_target);
}

DisplayError HWCDisplayBuiltIn::HistogramEvent(int fd, uint32_t blob_id) {
  histogram.notify_histogram_event(fd, blob_id);
  return kErrorNone;
}

int HWCDisplayBuiltIn::PostInit() {
  auto status = InitLayerStitch();
  if (!status) {
    DLOGW("Failed to initialize Layer Stitch context");
    // Disable layer stitch.
    disable_layer_stitch_ = true;
  }

  return 0;
}

void HWCDisplayBuiltIn::SetCpuPerfHintLargeCompCycle() {
  if (!cpu_hint_ || !perf_hint_large_comp_cycle_) {
    DLOGV_IF(kTagResources, "cpu_hint_ not initialized or property not set");
    return;
  }

  //Send large comp cycle hint only for fps >= 120
  if (active_refresh_rate_ < 120) {
    DLOGV_IF(kTagResources, "Skip large comp cycle hint for current fps - %u",
             active_refresh_rate_);
    return;
  }

  for (auto hwc_layer : layer_set_) {
    Layer *layer = hwc_layer->GetSDMLayer();
    if (layer->composition == kCompositionGPU) {
      DLOGV_IF(kTagResources, "Set perf hint for large comp cycle");
      int hwc_tid = gettid();
      cpu_hint_->ReqHintsOffload(kPerfHintLargeCompCycle, hwc_tid);
      break;
    }
  }
}

bool HWCDisplayBuiltIn::IsDisplayIdle() {
  // Notify only if this display is source of vsync.
  bool vsync_source = (callbacks_->GetVsyncSource() == id_);
  return vsync_source && display_idle_;
}

bool HWCDisplayBuiltIn::HasReadBackBufferSupport() {
  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);

  return fixed_info.readback_supported;
}

void HWCDisplayBuiltIn::EnablePartialUpdate() {
  if (partial_update_enabled_) {
    return;
  }

  if (!layer_stack_.flags.mask_present || enable_round_corner_) {
    return;
  }

  // Update PU status.
  partial_update_enabled_ = true;

  for (auto hwc_layer : layer_set_) {
    hwc_layer->SetPartialUpdate(partial_update_enabled_);
  }
  client_target_->SetPartialUpdate(partial_update_enabled_);
}

HWC2::Error HWCDisplayBuiltIn::NotifyDisplayCalibrationMode(bool in_calibration) {
  auto status = color_mode_->NotifyDisplayCalibrationMode(in_calibration);
  if (status != HWC2::Error::None) {
    DLOGE("Failed for notify QDCM mode = %d", in_calibration);
    return status;
  }

  return status;
}

uint32_t HWCDisplayBuiltIn::GetUpdatingAppLayersCount() {
  uint32_t updating_count = 0;

  for (uint i = 0; i < layer_stack_.layers.size(); i++) {
    auto layer = layer_stack_.layers.at(i);
    if (layer->composition == kCompositionGPUTarget) {
      break;
    }
    if (layer->flags.updating) {
      updating_count++;
    }
  }

  return updating_count;
}

HWC2::Error HWCDisplayBuiltIn::PresentAndOrGetValidateDisplayOutput(uint32_t *out_num_types,
                                                                    uint32_t *out_num_requests) {
  *out_num_types = UINT32(layer_changes_.size());
  *out_num_requests = UINT32(layer_requests_.size());

  auto status = (*out_num_types > 0) ? HWC2::Error::HasChanges : HWC2::Error::None;
  if (layer_stack_.block_on_fb) {
    return status;
  }

  // If a display is in internal Validate state, PresentDisplay can be triggered
  // if there is no dependency on GPU composed output in current draw cycle.
  shared_ptr<Fence> retire_fence = nullptr;
  auto result = Present(&retire_fence);
  if (result != HWC2::Error::None) {
    DLOGE("Commit failed: %d", result);
    return status;
  }

  // Store retire fence as client isn't concerned about it.
  retire_fence_ = retire_fence;
  // Validate State resets to kSkipValidate as part of PostCommit.
  // Restore it to kInternal Validate.
  validate_state_ = kInternalValidate;
  // As part of  PostCommit() commit state changes to NormalCommit.
  // Change it to InternalCommit so that next commit will be skipped.
  commit_state_ = kInternalCommit;
  layer_stack_.block_on_fb = true;

  return status;
}
}  // namespace sdm
