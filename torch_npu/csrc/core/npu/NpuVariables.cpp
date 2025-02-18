#include <iostream>
#include <map>
#include <string>

#include "torch_npu/csrc/core/npu/NpuVariables.h"
#include "torch_npu/csrc/core/npu/NPUException.h"
#include "torch_npu/csrc/core/npu/register/OptionsManager.h"

namespace c10_npu {
static SocVersion g_curSocVersion = SocVersion::UnsupportedSocVersion;

static std::map<std::string, SocVersion> socVersionMap = {
    {"Ascend910PremiumA", SocVersion::Ascend910PremiumA},
    {"Ascend910ProA", SocVersion::Ascend910ProA},
    {"Ascend910A", SocVersion::Ascend910A},
    {"Ascend910ProB", SocVersion::Ascend910ProB},
    {"Ascend910B", SocVersion::Ascend910B},
    {"Ascend310P1", SocVersion::Ascend310P1},
    {"Ascend310P2", SocVersion::Ascend310P2},
    {"Ascend310P3", SocVersion::Ascend310P3},
    {"Ascend310P4", SocVersion::Ascend310P4},
    {"Ascend910B1", SocVersion::Ascend910B1},
    {"Ascend910B2", SocVersion::Ascend910B2},
    {"Ascend910B3", SocVersion::Ascend910B3},
    {"Ascend910B4", SocVersion::Ascend910B4},
    {"Ascend310B1", SocVersion::Ascend310B1},
    {"Ascend310B2", SocVersion::Ascend310B2},
    {"Ascend310B3", SocVersion::Ascend310B3},
    {"Ascend910C1", SocVersion::Ascend910C1},
    {"Ascend910C2", SocVersion::Ascend910C2},
    {"Ascend910C3", SocVersion::Ascend910C3},
    {"Ascend910C4", SocVersion::Ascend910C4}};

void SetSocVersion(const char* const socVersion) {
  if (socVersion == nullptr ||
      g_curSocVersion != SocVersion::UnsupportedSocVersion) {
    return;
  }

  SocVersion curSocVersion = SocVersion::UnsupportedSocVersion;

  auto const& iter = socVersionMap.find(socVersion);
  if (iter != socVersionMap.end()) {
    curSocVersion = iter->second;
  } else {
    AT_ERROR("Unsupported soc version: ", socVersion);
  }

  g_curSocVersion = curSocVersion;
}

const SocVersion& GetSocVersion() { return g_curSocVersion; }

bool IsSupportInfNan() {
  return c10_npu::option::OptionsManager::CheckInfNanModeEnable() &&
         (GetSocVersion() >= SocVersion::Ascend910B1);
}
}  // namespace c10_npu

