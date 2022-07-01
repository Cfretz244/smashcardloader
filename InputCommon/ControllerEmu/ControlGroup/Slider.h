// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
class Slider : public ControlGroup
{
public:
  struct StateData
  {
    ControlState value{};
  };

  Slider(const std::string& name_, const std::string& ui_name_);
  explicit Slider(const std::string& name_);

  StateData GetState() const;

private:
  SettingValue<double> m_deadzone_setting;
};
}  // namespace ControllerEmu
