// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/FifoPlayer/FifoPlayer.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/VideoCommon.h"

// We need to include TextureDecoder.h for the texMem array.
// TODO: Move texMem somewhere else so this isn't an issue.
#include "VideoCommon/TextureDecoder.h"

namespace
{
class FifoPlaybackAnalyzer : public OpcodeDecoder::Callback
{
public:
  static void AnalyzeFrames(FifoDataFile* file, std::vector<AnalyzedFrameInfo>& frame_info);

  explicit FifoPlaybackAnalyzer(const u32* cpmem) : m_cpmem(cpmem) {}

  OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data)) {}
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value)) { GetCPState().LoadCPReg(command, value); }
  OPCODE_CALLBACK(void OnBP(u8 command, u32 value));
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size)) {}
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices,
                                          const u8* vertex_data));
  OPCODE_CALLBACK(void OnDisplayList(u32 address, u32 size)) {}
  OPCODE_CALLBACK(void OnNop(u32 count));
  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data)) {}

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size));

  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cpmem; }

  bool m_start_of_primitives = false;
  bool m_end_of_primitives = false;
  bool m_efb_copy = false;
  // Internal state, copied to above in OnCommand
  bool m_was_primitive = false;
  bool m_is_primitive = false;
  bool m_is_copy = false;
  bool m_is_nop = false;
  CPState m_cpmem;
};

void FifoPlaybackAnalyzer::AnalyzeFrames(FifoDataFile* file,
                                         std::vector<AnalyzedFrameInfo>& frame_info)
{
  FifoPlaybackAnalyzer analyzer(file->GetCPMem());
  frame_info.clear();
  frame_info.resize(file->GetFrameCount());

  for (u32 frame_no = 0; frame_no < file->GetFrameCount(); frame_no++)
  {
    const FifoFrameInfo& frame = file->GetFrame(frame_no);
    AnalyzedFrameInfo& analyzed = frame_info[frame_no];

    u32 offset = 0;

    u32 part_start = 0;
    CPState cpmem;

    while (offset < frame.fifoData.size())
    {
      const u32 cmd_size = OpcodeDecoder::RunCommand(&frame.fifoData[offset],
                                                     u32(frame.fifoData.size()) - offset, analyzer);

      if (analyzer.m_start_of_primitives)
      {
        // Start of primitive data for an object
        analyzed.AddPart(FramePartType::Commands, part_start, offset, analyzer.m_cpmem);
        part_start = offset;
        // Copy cpmem now, because end_of_primitives isn't triggered until the first opcode after
        // primitive data, and the first opcode might update cpmem
        std::memcpy(&cpmem, &analyzer.m_cpmem, sizeof(CPState));
      }
      if (analyzer.m_end_of_primitives)
      {
        // End of primitive data for an object, and thus end of the object
        analyzed.AddPart(FramePartType::PrimitiveData, part_start, offset, cpmem);
        part_start = offset;
      }

      offset += cmd_size;

      if (analyzer.m_efb_copy)
      {
        // We increase the offset beforehand, so that the trigger EFB copy command is included.
        analyzed.AddPart(FramePartType::EFBCopy, part_start, offset, analyzer.m_cpmem);
        part_start = offset;
      }
    }

    // The frame should end with an EFB copy, so part_start should have been updated to the end.
    ASSERT(part_start == frame.fifoData.size());
    ASSERT(offset == frame.fifoData.size());
  }
}

void FifoPlaybackAnalyzer::OnBP(u8 command, u32 value)
{
  if (command == BPMEM_TRIGGER_EFB_COPY)
    m_is_copy = true;
}

void FifoPlaybackAnalyzer::OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                              u32 vertex_size, u16 num_vertices,
                                              const u8* vertex_data)
{
  m_is_primitive = true;
}

void FifoPlaybackAnalyzer::OnNop(u32 count)
{
  m_is_nop = true;
}

void FifoPlaybackAnalyzer::OnCommand(const u8* data, u32 size)
{
  m_start_of_primitives = false;
  m_end_of_primitives = false;
  m_efb_copy = false;

  if (!m_is_nop)
  {
    if (m_is_primitive && !m_was_primitive)
      m_start_of_primitives = true;
    else if (m_was_primitive && !m_is_primitive)
      m_end_of_primitives = true;
    else if (m_is_copy)
      m_efb_copy = true;

    m_was_primitive = m_is_primitive;
  }
  m_is_primitive = false;
  m_is_copy = false;
  m_is_nop = false;
}
}  // namespace

bool IsPlayingBackFifologWithBrokenEFBCopies = false;

FifoPlayer::FifoPlayer()
{
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
}

FifoPlayer::~FifoPlayer()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

bool FifoPlayer::Open(const std::string& filename)
{
  Close();

  m_File = FifoDataFile::Load(filename, false);

  if (m_File)
  {
    FifoPlaybackAnalyzer::AnalyzeFrames(m_File.get(), m_FrameInfo);

    m_FrameRangeEnd = m_File->GetFrameCount() - 1;
  }

  if (m_FileLoadedCb)
    m_FileLoadedCb();

  return (m_File != nullptr);
}

void FifoPlayer::Close()
{
  m_File.reset();

  m_FrameRangeStart = 0;
  m_FrameRangeEnd = 0;
}

bool FifoPlayer::IsPlaying() const
{
  return GetFile() != nullptr && Core::IsRunning();
}

class FifoPlayer::CPUCore final : public CPUCoreBase
{
public:
  explicit CPUCore(FifoPlayer* parent) : m_parent(parent) {}
  CPUCore(const CPUCore&) = delete;
  ~CPUCore() {}
  CPUCore& operator=(const CPUCore&) = delete;

  void Init() override
  {
    IsPlayingBackFifologWithBrokenEFBCopies = m_parent->m_File->HasBrokenEFBCopies();
    // Without this call, we deadlock in initialization in dual core, as the FIFO is disabled and
    // thus ClearEfb()'s call to WaitForGPUInactive() never returns
    CPU::EnableStepping(false);

    m_parent->m_CurrentFrame = m_parent->m_FrameRangeStart;
    m_parent->LoadMemory();
  }

  void Shutdown() override { IsPlayingBackFifologWithBrokenEFBCopies = false; }
  void ClearCache() override
  {
    // Nothing to clear.
  }

  void SingleStep() override
  {
    // NOTE: AdvanceFrame() will get stuck forever in Dual Core because the FIFO
    //   is disabled by CPU::EnableStepping(true) so the frame never gets displayed.
    PanicAlertFmtT("Cannot SingleStep the FIFO. Use Frame Advance instead.");
  }

  const char* GetName() const override { return "FifoPlayer"; }
  void Run() override
  {
    while (CPU::GetState() == CPU::State::Running)
    {
      switch (m_parent->AdvanceFrame())
      {
      case CPU::State::PowerDown:
        CPU::Break();
        Host_Message(HostMessageID::WMUserStop);
        break;

      case CPU::State::Stepping:
        CPU::Break();
        Host_UpdateMainFrame();
        break;

      case CPU::State::Running:
        break;
      }
    }
  }

private:
  FifoPlayer* m_parent;
};

CPU::State FifoPlayer::AdvanceFrame()
{
  if (m_CurrentFrame > m_FrameRangeEnd)
  {
    if (!m_Loop)
      return CPU::State::PowerDown;

    // When looping, reload the contents of all the BP/CP/CF registers.
    // This ensures that each time the first frame is played back, the state of the
    // GPU is the same for each playback loop.
    m_CurrentFrame = m_FrameRangeStart;
    LoadRegisters();
    LoadTextureMemory();
    FlushWGP();
  }

  if (m_FrameWrittenCb)
    m_FrameWrittenCb();

  if (m_EarlyMemoryUpdates && m_CurrentFrame == m_FrameRangeStart)
    WriteAllMemoryUpdates();

  WriteFrame(m_File->GetFrame(m_CurrentFrame), m_FrameInfo[m_CurrentFrame]);

  ++m_CurrentFrame;
  return CPU::State::Running;
}

std::unique_ptr<CPUCoreBase> FifoPlayer::GetCPUCore()
{
  if (!m_File || m_File->GetFrameCount() == 0)
    return nullptr;

  return std::make_unique<CPUCore>(this);
}

void FifoPlayer::RefreshConfig()
{
  m_Loop = Config::Get(Config::MAIN_FIFOPLAYER_LOOP_REPLAY);
  m_EarlyMemoryUpdates = Config::Get(Config::MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES);
}

void FifoPlayer::SetFileLoadedCallback(CallbackFunc callback)
{
  m_FileLoadedCb = std::move(callback);

  // Trigger the callback immediatly if the file is already loaded.
  if (GetFile() != nullptr)
  {
    m_FileLoadedCb();
  }
}

bool FifoPlayer::IsRunningWithFakeVideoInterfaceUpdates() const
{
  if (!m_File || m_File->GetFrameCount() == 0)
  {
    return false;
  }

  return m_File->ShouldGenerateFakeVIUpdates();
}

u32 FifoPlayer::GetMaxObjectCount() const
{
  u32 result = 0;
  for (auto& frame : m_FrameInfo)
  {
    const u32 count = frame.part_type_counts[FramePartType::PrimitiveData];
    if (count > result)
      result = count;
  }
  return result;
}

u32 FifoPlayer::GetFrameObjectCount(u32 frame) const
{
  if (frame < m_FrameInfo.size())
  {
    return m_FrameInfo[frame].part_type_counts[FramePartType::PrimitiveData];
  }

  return 0;
}

u32 FifoPlayer::GetCurrentFrameObjectCount() const
{
  return GetFrameObjectCount(m_CurrentFrame);
}

void FifoPlayer::SetFrameRangeStart(u32 start)
{
  if (m_File)
  {
    const u32 lastFrame = m_File->GetFrameCount() - 1;
    if (start > lastFrame)
      start = lastFrame;

    m_FrameRangeStart = start;
    if (m_FrameRangeEnd < start)
      m_FrameRangeEnd = start;

    if (m_CurrentFrame < m_FrameRangeStart)
      m_CurrentFrame = m_FrameRangeStart;
  }
}

void FifoPlayer::SetFrameRangeEnd(u32 end)
{
  if (m_File)
  {
    const u32 lastFrame = m_File->GetFrameCount() - 1;
    if (end > lastFrame)
      end = lastFrame;

    m_FrameRangeEnd = end;
    if (m_FrameRangeStart > end)
      m_FrameRangeStart = end;

    if (m_CurrentFrame >= m_FrameRangeEnd)
      m_CurrentFrame = m_FrameRangeStart;
  }
}

FifoPlayer& FifoPlayer::GetInstance()
{
  static FifoPlayer instance;
  return instance;
}

void FifoPlayer::WriteFrame(const FifoFrameInfo& frame, const AnalyzedFrameInfo& info)
{
  // Core timing information
  m_CyclesPerFrame = static_cast<u64>(SystemTimers::GetTicksPerSecond()) *
                     VideoInterface::GetTargetRefreshRateDenominator() /
                     VideoInterface::GetTargetRefreshRateNumerator();
  m_ElapsedCycles = 0;
  m_FrameFifoSize = static_cast<u32>(frame.fifoData.size());

  u32 memory_update = 0;
  u32 object_num = 0;

  // Skip all memory updates if early memory updates are enabled, as we already wrote them
  if (m_EarlyMemoryUpdates)
  {
    memory_update = (u32)(frame.memoryUpdates.size());
  }

  for (const FramePart& part : info.parts)
  {
    bool show_part;

    if (part.m_type == FramePartType::PrimitiveData)
    {
      show_part = m_ObjectRangeStart <= object_num && object_num <= m_ObjectRangeEnd;
      object_num++;
    }
    else
    {
      // We always include commands and EFB copies, as commands from earlier objects still apply to
      // later ones (games generally do not reconfigure everything for each object)
      show_part = true;
    }

    if (show_part)
      WriteFramePart(part, &memory_update, frame);
  }

  FlushWGP();
  WaitForGPUInactive();
}

void FifoPlayer::WriteFramePart(const FramePart& part, u32* next_mem_update,
                                const FifoFrameInfo& frame)
{
  const u8* const data = frame.fifoData.data();

  u32 data_start = part.m_start;
  const u32 data_end = part.m_end;

  while (*next_mem_update < frame.memoryUpdates.size() && data_start < data_end)
  {
    const MemoryUpdate& memUpdate = frame.memoryUpdates[*next_mem_update];

    if (memUpdate.fifoPosition < data_end)
    {
      if (data_start < memUpdate.fifoPosition)
      {
        WriteFifo(data, data_start, memUpdate.fifoPosition);
        data_start = memUpdate.fifoPosition;
      }

      WriteMemory(memUpdate);

      ++*next_mem_update;
    }
    else
    {
      WriteFifo(data, data_start, data_end);
      data_start = data_end;
    }
  }

  if (data_start < data_end)
    WriteFifo(data, data_start, data_end);
}

void FifoPlayer::WriteAllMemoryUpdates()
{
  ASSERT(m_File);

  for (u32 frameNum = 0; frameNum < m_File->GetFrameCount(); ++frameNum)
  {
    const FifoFrameInfo& frame = m_File->GetFrame(frameNum);
    for (auto& update : frame.memoryUpdates)
    {
      WriteMemory(update);
    }
  }
}

void FifoPlayer::WriteMemory(const MemoryUpdate& memUpdate)
{
  u8* mem = nullptr;

  if (memUpdate.address & 0x10000000)
    mem = &Memory::m_pEXRAM[memUpdate.address & Memory::GetExRamMask()];
  else
    mem = &Memory::m_pRAM[memUpdate.address & Memory::GetRamMask()];

  std::copy(memUpdate.data.begin(), memUpdate.data.end(), mem);
}

void FifoPlayer::WriteFifo(const u8* data, u32 start, u32 end)
{
  u32 written = start;
  u32 lastBurstEnd = end - 1;

  // Write up to 256 bytes at a time
  while (written < end)
  {
    while (IsHighWatermarkSet())
    {
      if (CPU::GetState() != CPU::State::Running)
        break;
      CoreTiming::Idle();
      CoreTiming::Advance();
    }

    u32 burstEnd = std::min(written + 255, lastBurstEnd);

    std::copy(data + written, data + burstEnd, PowerPC::ppcState.gather_pipe_ptr);
    PowerPC::ppcState.gather_pipe_ptr += burstEnd - written;
    written = burstEnd;

    GPFifo::Write8(data[written++]);

    // Advance core timing
    u32 elapsedCycles = u32(((u64)written * m_CyclesPerFrame) / m_FrameFifoSize);
    u32 cyclesUsed = elapsedCycles - m_ElapsedCycles;
    m_ElapsedCycles = elapsedCycles;

    PowerPC::ppcState.downcount -= cyclesUsed;
    CoreTiming::Advance();
  }
}

void FifoPlayer::SetupFifo()
{
  WriteCP(CommandProcessor::CTRL_REGISTER, 0);   // disable read, BP, interrupts
  WriteCP(CommandProcessor::CLEAR_REGISTER, 7);  // clear overflow, underflow, metrics

  const FifoFrameInfo& frame = m_File->GetFrame(m_CurrentFrame);

  // Set fifo bounds
  WriteCP(CommandProcessor::FIFO_BASE_LO, frame.fifoStart);
  WriteCP(CommandProcessor::FIFO_BASE_HI, frame.fifoStart >> 16);
  WriteCP(CommandProcessor::FIFO_END_LO, frame.fifoEnd);
  WriteCP(CommandProcessor::FIFO_END_HI, frame.fifoEnd >> 16);

  // Set watermarks, high at 75%, low at 0%
  u32 hi_watermark = (frame.fifoEnd - frame.fifoStart) * 3 / 4;
  WriteCP(CommandProcessor::FIFO_HI_WATERMARK_LO, hi_watermark);
  WriteCP(CommandProcessor::FIFO_HI_WATERMARK_HI, hi_watermark >> 16);
  WriteCP(CommandProcessor::FIFO_LO_WATERMARK_LO, 0);
  WriteCP(CommandProcessor::FIFO_LO_WATERMARK_HI, 0);

  // Set R/W pointers to fifo start
  WriteCP(CommandProcessor::FIFO_RW_DISTANCE_LO, 0);
  WriteCP(CommandProcessor::FIFO_RW_DISTANCE_HI, 0);
  WriteCP(CommandProcessor::FIFO_WRITE_POINTER_LO, frame.fifoStart);
  WriteCP(CommandProcessor::FIFO_WRITE_POINTER_HI, frame.fifoStart >> 16);
  WriteCP(CommandProcessor::FIFO_READ_POINTER_LO, frame.fifoStart);
  WriteCP(CommandProcessor::FIFO_READ_POINTER_HI, frame.fifoStart >> 16);

  // Set fifo bounds
  WritePI(ProcessorInterface::PI_FIFO_BASE, frame.fifoStart);
  WritePI(ProcessorInterface::PI_FIFO_END, frame.fifoEnd);

  // Set write pointer
  WritePI(ProcessorInterface::PI_FIFO_WPTR, frame.fifoStart);
  FlushWGP();
  WritePI(ProcessorInterface::PI_FIFO_WPTR, frame.fifoStart);

  WriteCP(CommandProcessor::CTRL_REGISTER, 17);  // enable read & GP link
}

void FifoPlayer::ClearEfb()
{
  // Trigger a bogus EFB copy to clear the screen
  // The target address is 0, and there shouldn't be anything there,
  // but even if there is it should be loaded in by LoadTextureMemory afterwards
  X10Y10 tl = bpmem.copyTexSrcXY;
  tl.x = 0;
  tl.y = 0;
  LoadBPReg(BPMEM_EFB_TL, tl.hex);
  X10Y10 wh = bpmem.copyTexSrcWH;
  wh.x = EFB_WIDTH - 1;
  wh.y = EFB_HEIGHT - 1;
  LoadBPReg(BPMEM_EFB_WH, wh.hex);
  LoadBPReg(BPMEM_MIPMAP_STRIDE, 0x140);
  // The clear color and Z value have already been loaded via LoadRegisters()
  LoadBPReg(BPMEM_EFB_ADDR, 0);
  UPE_Copy copy = bpmem.triggerEFBCopy;
  copy.clamp_top = false;
  copy.clamp_bottom = false;
  copy.yuv = false;
  copy.target_pixel_format = static_cast<u32>(EFBCopyFormat::RGBA8) << 1;
  copy.gamma = 0;
  copy.half_scale = false;
  copy.scale_invert = false;
  copy.clear = true;
  copy.frame_to_field = FrameToField::Progressive;
  copy.copy_to_xfb = false;
  copy.intensity_fmt = false;
  copy.auto_conv = false;
  LoadBPReg(BPMEM_TRIGGER_EFB_COPY, copy.Hex);
  // Restore existing data - this only works at the start of the fifolog.
  // In practice most fifologs probably explicitly specify the size each time, but this is still
  // probably a good idea.
  LoadBPReg(BPMEM_EFB_TL, m_File->GetBPMem()[BPMEM_EFB_TL]);
  LoadBPReg(BPMEM_EFB_WH, m_File->GetBPMem()[BPMEM_EFB_WH]);
  LoadBPReg(BPMEM_MIPMAP_STRIDE, m_File->GetBPMem()[BPMEM_MIPMAP_STRIDE]);
  LoadBPReg(BPMEM_EFB_ADDR, m_File->GetBPMem()[BPMEM_EFB_ADDR]);
  // Wait for the EFB copy to finish.  That way, the EFB copy (which will be performed at a later
  // time) won't clobber any memory updates.
  FlushWGP();
  WaitForGPUInactive();
}

void FifoPlayer::LoadMemory()
{
  UReg_MSR newMSR;
  newMSR.DR = 1;
  newMSR.IR = 1;
  MSR.Hex = newMSR.Hex;
  PowerPC::ppcState.spr[SPR_IBAT0U] = 0x80001fff;
  PowerPC::ppcState.spr[SPR_IBAT0L] = 0x00000002;
  PowerPC::ppcState.spr[SPR_DBAT0U] = 0x80001fff;
  PowerPC::ppcState.spr[SPR_DBAT0L] = 0x00000002;
  PowerPC::ppcState.spr[SPR_DBAT1U] = 0xc0001fff;
  PowerPC::ppcState.spr[SPR_DBAT1L] = 0x0000002a;
  PowerPC::DBATUpdated();
  PowerPC::IBATUpdated();

  SetupFifo();
  LoadRegisters();
  ClearEfb();
  LoadTextureMemory();
  FlushWGP();
}

void FifoPlayer::LoadRegisters()
{
  const u32* regs = m_File->GetBPMem();
  for (int i = 0; i < FifoDataFile::BP_MEM_SIZE; ++i)
  {
    if (ShouldLoadBP(i))
      LoadBPReg(i, regs[i]);
  }

  regs = m_File->GetCPMem();
  LoadCPReg(MATINDEX_A, regs[MATINDEX_A]);
  LoadCPReg(MATINDEX_B, regs[MATINDEX_B]);
  LoadCPReg(VCD_LO, regs[VCD_LO]);
  LoadCPReg(VCD_HI, regs[VCD_HI]);

  for (int i = 0; i < CP_NUM_VAT_REG; ++i)
  {
    LoadCPReg(CP_VAT_REG_A + i, regs[CP_VAT_REG_A + i]);
    LoadCPReg(CP_VAT_REG_B + i, regs[CP_VAT_REG_B + i]);
    LoadCPReg(CP_VAT_REG_C + i, regs[CP_VAT_REG_C + i]);
  }

  for (int i = 0; i < CP_NUM_ARRAYS; ++i)
  {
    LoadCPReg(ARRAY_BASE + i, regs[ARRAY_BASE + i]);
    LoadCPReg(ARRAY_STRIDE + i, regs[ARRAY_STRIDE + i]);
  }

  regs = m_File->GetXFMem();
  for (int i = 0; i < FifoDataFile::XF_MEM_SIZE; i += 16)
    LoadXFMem16(i, &regs[i]);

  regs = m_File->GetXFRegs();
  for (int i = 0; i < FifoDataFile::XF_REGS_SIZE; ++i)
  {
    if (ShouldLoadXF(i))
      LoadXFReg(i, regs[i]);
  }
}

void FifoPlayer::LoadTextureMemory()
{
  static_assert(static_cast<size_t>(TMEM_SIZE) == static_cast<size_t>(FifoDataFile::TEX_MEM_SIZE),
                "TMEM_SIZE matches the size of texture memory in FifoDataFile");
  std::memcpy(texMem, m_File->GetTexMem(), FifoDataFile::TEX_MEM_SIZE);
}

void FifoPlayer::WriteCP(u32 address, u16 value)
{
  PowerPC::Write_U16(value, 0xCC000000 | address);
}

void FifoPlayer::WritePI(u32 address, u32 value)
{
  PowerPC::Write_U32(value, 0xCC003000 | address);
}

void FifoPlayer::FlushWGP()
{
  // Send 31 0s through the WGP
  for (int i = 0; i < 7; ++i)
    GPFifo::Write32(0);
  GPFifo::Write16(0);
  GPFifo::Write8(0);

  GPFifo::ResetGatherPipe();
}

void FifoPlayer::WaitForGPUInactive()
{
  // Sleep while the GPU is active
  while (!IsIdleSet() && CPU::GetState() != CPU::State::PowerDown)
  {
    CoreTiming::Idle();
    CoreTiming::Advance();
  }
}

void FifoPlayer::LoadBPReg(u8 reg, u32 value)
{
  GPFifo::Write8(0x61);  // load BP reg

  u32 cmd = (reg << 24) & 0xff000000;
  cmd |= (value & 0x00ffffff);
  GPFifo::Write32(cmd);
}

void FifoPlayer::LoadCPReg(u8 reg, u32 value)
{
  GPFifo::Write8(0x08);  // load CP reg
  GPFifo::Write8(reg);
  GPFifo::Write32(value);
}

void FifoPlayer::LoadXFReg(u16 reg, u32 value)
{
  GPFifo::Write8(0x10);                      // load XF reg
  GPFifo::Write32((reg & 0x0fff) | 0x1000);  // load 4 bytes into reg
  GPFifo::Write32(value);
}

void FifoPlayer::LoadXFMem16(u16 address, const u32* data)
{
  // Loads 16 * 4 bytes in xf memory starting at address
  GPFifo::Write8(0x10);                              // load XF reg
  GPFifo::Write32(0x000f0000 | (address & 0xffff));  // load 16 * 4 bytes into address
  for (int i = 0; i < 16; ++i)
    GPFifo::Write32(data[i]);
}

bool FifoPlayer::ShouldLoadBP(u8 address)
{
  switch (address)
  {
  case BPMEM_SETDRAWDONE:
  case BPMEM_PE_TOKEN_ID:
  case BPMEM_PE_TOKEN_INT_ID:
  case BPMEM_TRIGGER_EFB_COPY:
  case BPMEM_LOADTLUT1:
  case BPMEM_PRELOAD_MODE:
  case BPMEM_PERF1:
    return false;
  default:
    return true;
  }
}

bool FifoPlayer::ShouldLoadXF(u8 reg)
{
  // Ignore unknown addresses
  u16 address = reg + 0x1000;
  return !(address == XFMEM_UNKNOWN_1007 ||
           (address >= XFMEM_UNKNOWN_GROUP_1_START && address <= XFMEM_UNKNOWN_GROUP_1_END) ||
           (address >= XFMEM_UNKNOWN_GROUP_2_START && address <= XFMEM_UNKNOWN_GROUP_2_END) ||
           (address >= XFMEM_UNKNOWN_GROUP_3_START && address <= XFMEM_UNKNOWN_GROUP_3_END));
}

bool FifoPlayer::IsIdleSet()
{
  CommandProcessor::UCPStatusReg status =
      PowerPC::Read_U16(0xCC000000 | CommandProcessor::STATUS_REGISTER);
  return status.CommandIdle;
}

bool FifoPlayer::IsHighWatermarkSet()
{
  CommandProcessor::UCPStatusReg status =
      PowerPC::Read_U16(0xCC000000 | CommandProcessor::STATUS_REGISTER);
  return status.OverflowHiWatermark;
}
