/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "bluetooth-a2dp"

#include "btif/include/bta_av_co_peer.h"

#include <bluetooth/log.h>

#include "bta/include/bta_av_api.h"
#include "osi/include/properties.h"
#include "device/include/interop.h"
#include "btif/include/btif_config.h"

using namespace bluetooth;

// Macro to convert BTA AV audio handle to index and vice versa
#define BTA_AV_CO_AUDIO_HANDLE_TO_INDEX(bta_av_handle) \
  (((bta_av_handle) & (~BTA_AV_CHNL_MSK)) - 1)
#define BTA_AV_CO_AUDIO_INDEX_TO_HANDLE(index) \
  (((index) + 1) | BTA_AV_CHNL_AUDIO)

BtaAvCoPeer::BtaAvCoPeer()
    : addr(RawAddress::kEmpty),
      num_sinks(0),
      num_sources(0),
      num_seps(0),
      num_rx_sinks(0),
      num_rx_sources(0),
      num_sup_sinks(0),
      num_sup_sources(0),
      p_sink(nullptr),
      p_source(nullptr),
      codec_config{},
      acceptor(false),
      reconfig_needed(false),
      opened(false),
      mtu(0),
      uuid_to_connect(0),
      bta_av_handle_(0),
      codecs_(nullptr),
      content_protect_active_(false) {
  Reset(0);
}

void BtaAvCoPeer::Init(
    const std::vector<btav_a2dp_codec_config_t>& codec_priorities) {
  Reset(bta_av_handle_);
  // Reset the current config
  codecs_ = new A2dpCodecs(codec_priorities);
  codecs_->init();
  A2DP_InitDefaultCodec(codec_config);
}

void BtaAvCoPeer::Reset(tBTA_AV_HNDL bta_av_handle) {
  addr = RawAddress::kEmpty;
  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(sinks); i++) {
    BtaAvCoSep& sink = sinks[i];
    sink.Reset();
  }
  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(sources); i++) {
    BtaAvCoSep& source = sources[i];
    source.Reset();
  }
  num_sinks = 0;
  num_sources = 0;
  num_seps = 0;
  num_rx_sinks = 0;
  num_rx_sources = 0;
  num_sup_sinks = 0;
  num_sup_sources = 0;
  p_sink = nullptr;
  p_source = nullptr;
  memset(codec_config, 0, sizeof(codec_config));
  acceptor = false;
  reconfig_needed = false;
  opened = false;
  mtu = 0;
  uuid_to_connect = 0;

  bta_av_handle_ = bta_av_handle;
  delete codecs_;
  codecs_ = nullptr;
  content_protect_active_ = false;
}

void BtaAvCoPeerCache::Init(
    const std::vector<btav_a2dp_codec_config_t>& codec_priorities,
    std::vector<btav_a2dp_codec_info_t>* supported_codecs) {
  std::lock_guard<std::recursive_mutex> lock(codec_lock_);

  codec_priorities_ = codec_priorities;

  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(peers_); i++) {
    BtaAvCoPeer* p_peer = &peers_[i];
    p_peer->Init(codec_priorities);
  }
}

void BtaAvCoPeerCache::Reset() {
  codec_priorities_.clear();

  // Reset the peers and initialize the handles
  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(peers_); i++) {
    BtaAvCoPeer* p_peer = &peers_[i];
    p_peer->Reset(BTA_AV_CO_AUDIO_INDEX_TO_HANDLE(i));
  }
}

BtaAvCoPeer* BtaAvCoPeerCache::FindPeer(const RawAddress& peer_address) {
  for (size_t i = 0; i < BTA_AV_CO_NUM_ELEMENTS(peers_); i++) {
    BtaAvCoPeer* p_peer = &peers_[i];
    if (p_peer->addr == peer_address) {
      return p_peer;
    }
  }
  return nullptr;
}

BtaAvCoSep* BtaAvCoPeerCache::FindPeerSource(
    BtaAvCoPeer* p_peer, btav_a2dp_codec_index_t codec_index,
    const uint8_t content_protect_flag) {
  if (codec_index == BTAV_A2DP_CODEC_INDEX_MAX) {
    log::warn("invalid codec index for peer {}", p_peer->addr);
    return nullptr;
  }

  // Find the peer Source for the codec
  for (size_t index = 0; index < p_peer->num_sup_sources; index++) {
    BtaAvCoSep* p_source = &p_peer->sources[index];
    btav_a2dp_codec_index_t peer_codec_index =
        A2DP_SinkCodecIndex(p_source->codec_caps);
    if (peer_codec_index != codec_index) {
      continue;
    }
    if (!AudioSepHasContentProtection(p_source, content_protect_flag)) {
      log::verbose(
          "peer Source for codec {} does not support Content Protection",
          A2DP_CodecIndexStr(codec_index));
      continue;
    }
    return p_source;
  }
  return nullptr;
}

bool bta_av_co_check_peer_eligible_for_aac_codec(const BtaAvCoPeer* p_peer){
  char remote_name[248] = "";
  bool aac_support = false;
  log::verbose("{}", __func__);
  if (interop_match_addr(INTEROP_ENABLE_AAC_CODEC, &p_peer->addr)) {
        log::verbose("AAC is supported for this WL remote device");
        aac_support = true;
  } else {
        log::verbose("RD is not present in address based check for AAC WL.");
  }
  log::verbose("returned AAC support {}", aac_support);
  return aac_support;
}

BtaAvCoSep* BtaAvCoPeerCache::FindPeerSink(BtaAvCoPeer* p_peer,
                                           btav_a2dp_codec_index_t codec_index,
                                           const uint8_t content_protect_flag) {
  if (codec_index == BTAV_A2DP_CODEC_INDEX_MAX) {
    log::warn("invalid codec index for peer {}", p_peer->addr);
    return nullptr;
  }


  // Find the peer Sink for the codec
  for (size_t index = 0; index < p_peer->num_sup_sinks; index++) {
    BtaAvCoSep* p_sink = &p_peer->sinks[index];
    btav_a2dp_codec_index_t peer_codec_index =
        A2DP_SourceCodecIndex(p_sink->codec_caps);
    if (peer_codec_index != codec_index) {
      continue;
    }
    if (!AudioSepHasContentProtection(p_sink, content_protect_flag)) {
      log::warn("invalid codec index for peer {}", p_peer->addr);
      continue;
    }
    if(codec_index == BTAV_A2DP_CODEC_INDEX_SOURCE_AAC) {
      std::string remote_bdstr = p_peer->addr.ToString();
      log::verbose("Remote AAC VBR cap is {}",p_sink->codec_caps[6]);
      bool remote_vbr = (p_sink->codec_caps[6] >> 7) & 1;
      if(remote_vbr) {
          btif_config_set_str(remote_bdstr, BTIF_STORAGE_KEY_FOR_AAC_VBR, "true");
      }
      else {
          btif_config_set_str(remote_bdstr, BTIF_STORAGE_KEY_FOR_AAC_VBR, "false");
      }
      log::verbose("Checking if peer is in AAC WL");
      bool vbr_bl = false;
      bool vbr_supp = osi_property_get_bool("persist.vendor.qcom.bluetooth.aac_vbr_ctl.enabled",
                   true);
      log::verbose("AAC VBR prop value is {}", vbr_supp);

      if(remote_vbr) {
        if (interop_match_addr(INTEROP_DISABLE_AAC_VBR_CODEC, &p_peer->addr)) {
          log::verbose("AAC VBR is not supported for this BL remote device");
          vbr_bl = true;
        }
      }

      if(remote_vbr) {
        log::verbose("Remote supports AAC VBR");
        if (!vbr_bl) {
          log::verbose("AAC VBR is enabled, show AAC support in selectable capability");
        } else {
           log::verbose("peer {}, checking in AAC BL", ADDRESS_TO_LOGGABLE_CSTR(p_peer->addr));
           if (!bta_av_co_check_peer_eligible_for_aac_codec(p_peer)) {
             log::verbose("peer {}, AAC not supported for this remote",
                           ADDRESS_TO_LOGGABLE_CSTR(p_peer->addr));
             return nullptr;
           }
        }
      } else {
         log::verbose("Remote does not support AAC VBR, check in AAC WL");
         if (!bta_av_co_check_peer_eligible_for_aac_codec(p_peer)) {
             log::verbose("peer {}, AAC not supported for this remote",
                           ADDRESS_TO_LOGGABLE_CSTR(p_peer->addr));
             return nullptr;
         }
     }
    }
    return p_sink;
  }
  return nullptr;
}

BtaAvCoPeer* BtaAvCoPeerCache::FindPeer(tBTA_AV_HNDL bta_av_handle) {
  uint8_t index;

  index = BTA_AV_CO_AUDIO_HANDLE_TO_INDEX(bta_av_handle);

  log::verbose("bta_av_handle = 0x{:x} index = {}", bta_av_handle, index);

  // Sanity check
  if (index >= BTA_AV_CO_NUM_ELEMENTS(peers_)) {
    log::error("peer index {} for BTA AV handle 0x{:x} is out of bounds", index,
               bta_av_handle);
    return nullptr;
  }

  return &peers_[index];
}

BtaAvCoPeer* BtaAvCoPeerCache::FindPeerAndUpdate(
    tBTA_AV_HNDL bta_av_handle, const RawAddress& peer_address) {
  log::verbose("peer {} bta_av_handle = 0x{:x}", peer_address, bta_av_handle);

  BtaAvCoPeer* p_peer = FindPeer(bta_av_handle);
  if (p_peer == nullptr) {
    log::error("peer entry for BTA AV handle 0x{:x} peer {} not found",
               bta_av_handle, peer_address);
    return nullptr;
  }

  log::verbose("peer {} bta_av_handle = 0x{:x} previous address {}",
               peer_address, bta_av_handle, p_peer->addr);
  p_peer->addr = peer_address;
  return p_peer;
}

uint16_t BtaAvCoPeerCache::FindPeerUuid(tBTA_AV_HNDL bta_av_handle) {
  BtaAvCoPeer* p_peer = FindPeer(bta_av_handle);
  if (p_peer == nullptr) {
    return 0;
  }
  return p_peer->uuid_to_connect;
}

bool ContentProtectIsScmst(const uint8_t* p_protect_info) {
  log::verbose("");

  if (*p_protect_info >= AVDT_CP_LOSC) {
    uint16_t cp_id;
    p_protect_info++;
    STREAM_TO_UINT16(cp_id, p_protect_info);
    if (cp_id == AVDT_CP_SCMS_T_ID) {
      log::verbose("SCMS-T found");
      return true;
    }
  }
  return false;
}

bool AudioProtectHasScmst(uint8_t num_protect, const uint8_t* p_protect_info) {
  log::verbose("");
  while (num_protect--) {
    if (ContentProtectIsScmst(p_protect_info)) return true;
    // Move to the next Content Protect schema
    p_protect_info += *p_protect_info + 1;
  }
  log::verbose("SCMS-T not found");
  return false;
}

bool AudioSepHasContentProtection(const BtaAvCoSep* p_sep,
                                  const uint8_t content_protect_flag) {
  log::verbose("");

  // Check if content protection is enabled for this stream
  if (content_protect_flag != AVDT_CP_SCMS_COPY_FREE) {
    return AudioProtectHasScmst(p_sep->num_protect, p_sep->protect_info);
  }

  log::verbose("not required");
  return true;
}
