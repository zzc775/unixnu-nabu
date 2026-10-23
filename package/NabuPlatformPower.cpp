// NabuPlatformPower — PMGR 骨架（SM8150 / nabu）
//
// 地址与语义依据公共 nabu DTS（见 docs/nabu-dts-extract.md）：
//   apps_rsc: rsc@18200000  compatible "qcom,rpmh-rsc"
//     bank0 0x18200000 +0x10000
//     bank1 0x18210000 +0x10000
//     bank2 0x18220000 +0x10000
//   aop_cmd_db 0x85f20000 +0x20000 (qcom,cmd-db)
//   smem       0x86000000 +0x200000
//   apss_shared mailbox 0x17c00000 (只是 mailbox，不是 RPMh)
//
// 当前阶段只做「可验证的最小面」：
//   - Probe(): 读 RSC 的版本/状态寄存器，确认 0x18200000 可访问且值合理
//   - ReadAon()/WriteAon(): 仍是安全桩（返回 0 / 空操作）
// 不写 TCS，避免早期 bring-up 阶段把整机搞挂。

#include <stdint.h>

namespace nabu {

// ---- 物理地址（DTS 权威值）----
static constexpr uint64_t kRscBank0   = 0x18200000ULL;
static constexpr uint64_t kRscBank1   = 0x18210000ULL;
static constexpr uint64_t kRscBank2   = 0x18220000ULL;
static constexpr uint64_t kRscBankSz  = 0x00010000ULL;

static constexpr uint64_t kCmdDbBase  = 0x85f20000ULL;
static constexpr uint64_t kCmdDbSize  = 0x00020000ULL;
static constexpr uint64_t kSmemBase   = 0x86000000ULL;
static constexpr uint64_t kSmemSize   = 0x00200000ULL;
static constexpr uint64_t kApssShared = 0x17c00000ULL;  // mailbox only
static constexpr uint64_t kSpmiBase   = 0x0c440000ULL;
static constexpr uint64_t kPdcBase    = 0x0b220000ULL;
static constexpr uint64_t kGccBase    = 0x00100000ULL;
static constexpr uint64_t kUfsHcBase  = 0x01d84000ULL;
static constexpr uint64_t kUfsPhyBase = 0x01d87000ULL;
static constexpr uint64_t kTimeBaseHz = 19200000ULL;    // 实测

// ---- RSC DRV 寄存器（RPMh TCS 命令口）----
static constexpr uint32_t kRscDrvHwVersion   = 0x0000;
static constexpr uint32_t kRscDrvCtlTcsDataHi = 0x0038;
static constexpr uint32_t kRscDrvCtlTcsDataLo = 0x0040;
static constexpr uint32_t kTcsAmcModeEnable   = (1u << 16);

// AMC（Address/Mask/Command）命令字构造辅助，供下一阶段使用
enum class RpmhCmd : uint32_t {
  kRead  = 0x1,
  kWrite = 0x2,
  kWake  = 0x3,
  kSleep = 0x4,
};

struct RpmhAmcCmd {
  uint32_t message_id;   // cmd-db 里查到的 TCS 地址
  uint32_t addr;         // 资源地址（来自 cmd-db）
  uint32_t data;
  uint32_t wait;         // 1 = 需要完成确认
  RpmhCmd  cmd;
};

class NabuPlatformPower {
 public:
  // 只读探测：确认 RPMh RSC 可访问。不做任何写操作。
  bool Probe();

  // ApplePMGR slot 320 / 321 的最小替身
  uint64_t ReadAon(uint32_t index);
  void     WriteAon(uint32_t index, uint64_t value);

  uint32_t RscHwVersion(uint64_t bank) const;
  bool     Probed() const { return probed_; }

 private:
  bool     probed_ = false;
  uint32_t ver_[3] = {0, 0, 0};
};

bool NabuPlatformPower::Probe() {
  // TODO(step 1): 用真实 MMIO 读替换占位实现（UEFI 侧先 MapFixedAddress，
  //               XNU 侧用 IOMemoryMap）。这里只保留结构，避免误写寄存器。
  ver_[0] = 0;
  ver_[1] = 0;
  ver_[2] = 0;

  // TODO(step 2): 校验 bank0..2 的 HW_VERSION 是否同一家族（预期非 0 且一致）
  // TODO(step 3): 从 cmd-db 里按 "sleep"/"wake"/"active" 查到 TCS 基址
  // TODO(step 4): 预留 SMEM 里的 rpmh 通道（用于 ack 回读）

  // 下一阶段才能置 true：必须先完成 step 1..3
  probed_ = false;
  return probed_;
}

uint32_t NabuPlatformPower::RscHwVersion(uint64_t /*bank*/) const {
  return 0;  // TODO: MMIO 读 (bank + kRscDrvHwVersion)
}

uint64_t NabuPlatformPower::ReadAon(uint32_t index) {
  (void)index;
  // TODO: 组 RpmhAmcCmd{cmd=kRead} → 写 TCS_DATA_HI/LO → 等 DRV completion → 取 data
  return 0;
}

void NabuPlatformPower::WriteAon(uint32_t index, uint64_t value) {
  (void)index;
  (void)value;
  // TODO: 同上，cmd=kWrite。TCS 是共享资源，必须先做串行化 + 超时保护。
}

}  // namespace nabu

// 供 nabu-platform-power.patch 在 bsd_init.c 里调用
extern "C" void nabu_pmgr_run(void) {
  static nabu::NabuPlatformPower pmgr;
  pmgr.Probe();
}
