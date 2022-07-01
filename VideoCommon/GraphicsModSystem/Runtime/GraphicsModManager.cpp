// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"

#include <string>
#include <string_view>
#include <variant>

#include "Common/Logging/Log.h"
#include "Common/VariantUtil.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"
#include "VideoCommon/TextureInfo.h"

class GraphicsModManager::DecoratedAction final : public GraphicsModAction
{
public:
  DecoratedAction(std::unique_ptr<GraphicsModAction> action, GraphicsModConfig mod)
      : m_action_impl(std::move(action)), m_mod(std::move(mod))
  {
  }
  void OnDrawStarted(bool* skip) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnDrawStarted(skip);
  }
  void OnEFB(bool* skip, u32 texture_width, u32 texture_height, u32* scaled_width,
             u32* scaled_height) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnEFB(skip, texture_width, texture_height, scaled_width, scaled_height);
  }
  void OnProjection(Common::Matrix44* matrix) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnProjection(matrix);
  }
  void OnProjectionAndTexture(Common::Matrix44* matrix) override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnProjectionAndTexture(matrix);
  }
  void OnTextureLoad() override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnTextureLoad();
  }
  void OnFrameEnd() override
  {
    if (!m_mod.m_enabled)
      return;
    m_action_impl->OnFrameEnd();
  }

private:
  GraphicsModConfig m_mod;
  std::unique_ptr<GraphicsModAction> m_action_impl;
};

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetProjectionActions(ProjectionType projection_type) const
{
  if (const auto it = m_projection_target_to_actions.find(projection_type);
      it != m_projection_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetProjectionTextureActions(ProjectionType projection_type,
                                                const std::string& texture_name) const
{
  const auto lookup = fmt::format("{}_{}", texture_name, static_cast<int>(projection_type));
  if (const auto it = m_projection_texture_target_to_actions.find(lookup);
      it != m_projection_texture_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetDrawStartedActions(const std::string& texture_name) const
{
  if (const auto it = m_draw_started_target_to_actions.find(texture_name);
      it != m_draw_started_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>&
GraphicsModManager::GetTextureLoadActions(const std::string& texture_name) const
{
  if (const auto it = m_load_target_to_actions.find(texture_name);
      it != m_load_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>& GraphicsModManager::GetEFBActions(const FBInfo& efb) const
{
  if (const auto it = m_efb_target_to_actions.find(efb); it != m_efb_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

const std::vector<GraphicsModAction*>& GraphicsModManager::GetXFBActions(const FBInfo& xfb) const
{
  if (const auto it = m_efb_target_to_actions.find(xfb); it != m_efb_target_to_actions.end())
  {
    return it->second;
  }

  return m_default;
}

void GraphicsModManager::Load(const GraphicsModGroupConfig& config)
{
  Reset();

  const auto& mods = config.GetMods();

  std::map<std::string, std::vector<GraphicsTargetConfig>> group_to_targets;
  for (const auto& mod : mods)
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
    {
      if (m_groups.find(group.m_name) != m_groups.end())
      {
        WARN_LOG_FMT(
            VIDEO,
            "Specified graphics mod group '{}' for mod '{}' is already specified by another mod.",
            group.m_name, mod.m_title);
      }
      m_groups.insert(group.m_name);

      const auto internal_group = fmt::format("{}.{}", mod.m_title, group.m_name);
      for (const GraphicsTargetConfig& target : group.m_targets)
      {
        group_to_targets[group.m_name].push_back(target);
        group_to_targets[internal_group].push_back(target);
      }
    }
  }

  for (const auto& mod : mods)
  {
    for (const GraphicsModFeatureConfig& feature : mod.m_features)
    {
      const auto create_action = [](const std::string_view& action_name,
                                    const picojson::value& json_data,
                                    GraphicsModConfig mod) -> std::unique_ptr<GraphicsModAction> {
        auto action = GraphicsModActionFactory::Create(action_name, json_data);
        if (action == nullptr)
        {
          return nullptr;
        }
        return std::make_unique<DecoratedAction>(std::move(action), std::move(mod));
      };

      const auto internal_group = fmt::format("{}.{}", mod.m_title, feature.m_group);

      const auto add_target = [&](const GraphicsTargetConfig& target, GraphicsModConfig mod) {
        auto action = create_action(feature.m_action, feature.m_action_data, std::move(mod));
        if (action == nullptr)
        {
          WARN_LOG_FMT(VIDEO, "Failed to create action '{}' for group '{}'.", feature.m_action,
                       feature.m_group);
          return;
        }
        m_actions.push_back(std::move(action));
        std::visit(
            overloaded{
                [&](const DrawStartedTextureTarget& the_target) {
                  m_draw_started_target_to_actions[the_target.m_texture_info_string].push_back(
                      m_actions.back().get());
                },
                [&](const LoadTextureTarget& the_target) {
                  m_load_target_to_actions[the_target.m_texture_info_string].push_back(
                      m_actions.back().get());
                },
                [&](const EFBTarget& the_target) {
                  FBInfo info;
                  info.m_height = the_target.m_height;
                  info.m_width = the_target.m_width;
                  info.m_texture_format = the_target.m_texture_format;
                  m_efb_target_to_actions[info].push_back(m_actions.back().get());
                },
                [&](const XFBTarget& the_target) {
                  FBInfo info;
                  info.m_height = the_target.m_height;
                  info.m_width = the_target.m_width;
                  info.m_texture_format = the_target.m_texture_format;
                  m_xfb_target_to_actions[info].push_back(m_actions.back().get());
                },
                [&](const ProjectionTarget& the_target) {
                  if (the_target.m_texture_info_string)
                  {
                    const auto lookup = fmt::format("{}_{}", *the_target.m_texture_info_string,
                                                    static_cast<int>(the_target.m_projection_type));
                    m_projection_texture_target_to_actions[lookup].push_back(
                        m_actions.back().get());
                  }
                  else
                  {
                    m_projection_target_to_actions[the_target.m_projection_type].push_back(
                        m_actions.back().get());
                  }
                },
            },
            target);
      };

      // Prefer groups in the pack over groups from another pack
      if (const auto local_it = group_to_targets.find(internal_group);
          local_it != group_to_targets.end())
      {
        for (const GraphicsTargetConfig& target : local_it->second)
        {
          add_target(target, mod);
        }
      }
      else if (const auto global_it = group_to_targets.find(feature.m_group);
               global_it != group_to_targets.end())
      {
        for (const GraphicsTargetConfig& target : global_it->second)
        {
          add_target(target, mod);
        }
      }
      else
      {
        WARN_LOG_FMT(VIDEO, "Specified graphics mod group '{}' was not found for mod '{}'",
                     feature.m_group, mod.m_title);
      }
    }
  }
}

void GraphicsModManager::EndOfFrame()
{
  for (auto&& action : m_actions)
  {
    action->OnFrameEnd();
  }
}

void GraphicsModManager::Reset()
{
  m_actions.clear();
  m_groups.clear();
  m_projection_target_to_actions.clear();
  m_projection_texture_target_to_actions.clear();
  m_draw_started_target_to_actions.clear();
  m_load_target_to_actions.clear();
  m_efb_target_to_actions.clear();
  m_xfb_target_to_actions.clear();
}
