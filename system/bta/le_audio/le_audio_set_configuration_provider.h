/*
 *  Copyright (c) 2022 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#pragma once

#include "le_audio_types.h"

namespace bluetooth::le_audio {

/* Audio set configurations provider interface. */
class AudioSetConfigurationProvider {
 public:
  AudioSetConfigurationProvider();
  virtual ~AudioSetConfigurationProvider() = default;
  static AudioSetConfigurationProvider* Get();
  static void Initialize(types::CodecLocation location);
  static void DebugDump(int fd);
  static void Cleanup();
  virtual const set_configurations::AudioSetConfigurations* GetConfigurations(
      ::bluetooth::le_audio::types::LeAudioContextType content_type) const;
  virtual bool CheckEnhancedGamingConfig(
      const set_configurations::AudioSetConfiguration& set_configuration) const;
  virtual bool CheckConfigurationIsBiDirSwb(
      const set_configurations::AudioSetConfiguration& set_configuration) const;
  virtual bool CheckConfigurationIsDualBiDirSwb(
      const set_configurations::AudioSetConfiguration& set_configuration) const;

 private:
  struct impl;
  std::unique_ptr<impl> pimpl_;
};

}  // namespace bluetooth::le_audio
