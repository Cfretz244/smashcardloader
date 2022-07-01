// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/TextureInfo.h"
#include "VideoCommon/XFMemory.h"

class GraphicsModGroupConfig;
class GraphicsModManager
{
public:
  const std::vector<GraphicsModAction*>& GetProjectionActions(ProjectionType projection_type) const;
  const std::vector<GraphicsModAction*>&
  GetProjectionTextureActions(ProjectionType projection_type,
                              const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>&
  GetDrawStartedActions(const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>&
  GetTextureLoadActions(const std::string& texture_name) const;
  const std::vector<GraphicsModAction*>& GetEFBActions(const FBInfo& efb) const;
  const std::vector<GraphicsModAction*>& GetXFBActions(const FBInfo& xfb) const;

  void Load(const GraphicsModGroupConfig& config);

  void EndOfFrame();

private:
  void Reset();

  class DecoratedAction;

  static inline const std::vector<GraphicsModAction*> m_default = {};
  std::list<std::unique_ptr<GraphicsModAction>> m_actions;
  std::unordered_map<ProjectionType, std::vector<GraphicsModAction*>>
      m_projection_target_to_actions;
  std::unordered_map<std::string, std::vector<GraphicsModAction*>>
      m_projection_texture_target_to_actions;
  std::unordered_map<std::string, std::vector<GraphicsModAction*>> m_draw_started_target_to_actions;
  std::unordered_map<std::string, std::vector<GraphicsModAction*>> m_load_target_to_actions;
  std::unordered_map<FBInfo, std::vector<GraphicsModAction*>, FBInfoHasher> m_efb_target_to_actions;
  std::unordered_map<FBInfo, std::vector<GraphicsModAction*>, FBInfoHasher> m_xfb_target_to_actions;

  std::unordered_set<std::string> m_groups;
};
