// Copyright 2011 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/FifoPlayer/FifoRecorder.h"

#include <algorithm>
#include <cstring>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"

#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"

#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/XFStructs.h"

class FifoRecorder::FifoRecordAnalyzer : public OpcodeDecoder::Callback
{
public:
  explicit FifoRecordAnalyzer(FifoRecorder* owner) : m_owner(owner) {}
  explicit FifoRecordAnalyzer(FifoRecorder* owner, const u32* cpmem)
      : m_owner(owner), m_cpmem(cpmem)
  {
  }

  OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data)) {}
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value)) { GetCPState().LoadCPReg(command, value); }
  OPCODE_CALLBACK(void OnBP(u8 command, u32 value)) {}
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size));
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices,
                                          const u8* vertex_data));
  OPCODE_CALLBACK(void OnDisplayList(u32 address, u32 size))
  {
    WARN_LOG_FMT(VIDEO,
                 "Unhandled display list call {:08x} {:08x}; should have been inlined earlier",
                 address, size);
  }
  OPCODE_CALLBACK(void OnNop(u32 count)) {}
  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data)) {}

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size)) {}

  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cpmem; }

private:
  void ProcessVertexComponent(CPArray array_index, VertexComponentFormat array_type,
                              u32 component_offset, u32 vertex_size, u16 num_vertices,
                              const u8* vertex_data);

  FifoRecorder* const m_owner;
  CPState m_cpmem;
};

void FifoRecorder::FifoRecordAnalyzer::OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size)
{
  const u32 load_address = m_cpmem.array_bases[array] + m_cpmem.array_strides[array] * index;

  m_owner->UseMemory(load_address, size * sizeof(u32), MemoryUpdate::XF_DATA);
}

// TODO: The following code is copied with modifications from VertexLoaderBase.
// Surely there's a better solution?
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"

void FifoRecorder::FifoRecordAnalyzer::OnPrimitiveCommand(OpcodeDecoder::Primitive primitive,
                                                          u8 vat, u32 vertex_size, u16 num_vertices,
                                                          const u8* vertex_data)
{
  const auto& vtx_desc = m_cpmem.vtx_desc;
  const auto& vtx_attr = m_cpmem.vtx_attr[vat];

  u32 offset = 0;

  if (vtx_desc.low.PosMatIdx)
    offset++;
  for (auto texmtxidx : vtx_desc.low.TexMatIdx)
  {
    if (texmtxidx)
      offset++;
  }
  const u32 pos_size = VertexLoader_Position::GetSize(vtx_desc.low.Position, vtx_attr.g0.PosFormat,
                                                      vtx_attr.g0.PosElements);
  ProcessVertexComponent(CPArray::Position, vtx_desc.low.Position, offset, vertex_size,
                         num_vertices, vertex_data);
  offset += pos_size;

  const u32 norm_size =
      VertexLoader_Normal::GetSize(vtx_desc.low.Normal, vtx_attr.g0.NormalFormat,
                                   vtx_attr.g0.NormalElements, vtx_attr.g0.NormalIndex3);
  ProcessVertexComponent(CPArray::Normal, vtx_desc.low.Position, offset, vertex_size, num_vertices,
                         vertex_data);
  offset += norm_size;

  for (u32 i = 0; i < vtx_desc.low.Color.Size(); i++)
  {
    const u32 color_size =
        VertexLoader_Color::GetSize(vtx_desc.low.Color[i], vtx_attr.GetColorFormat(i));
    ProcessVertexComponent(CPArray::Color0 + i, vtx_desc.low.Position, offset, vertex_size,
                           num_vertices, vertex_data);
    offset += color_size;
  }
  for (u32 i = 0; i < vtx_desc.high.TexCoord.Size(); i++)
  {
    const u32 tc_size = VertexLoader_TextCoord::GetSize(
        vtx_desc.high.TexCoord[i], vtx_attr.GetTexFormat(i), vtx_attr.GetTexElements(i));
    ProcessVertexComponent(CPArray::TexCoord0 + i, vtx_desc.low.Position, offset, vertex_size,
                           num_vertices, vertex_data);
    offset += tc_size;
  }

  ASSERT(offset == vertex_size);
}

// If a component is indexed, the array it indexes into for data must be saved.
void FifoRecorder::FifoRecordAnalyzer::ProcessVertexComponent(CPArray array_index,
                                                              VertexComponentFormat array_type,
                                                              u32 component_offset, u32 vertex_size,
                                                              u16 num_vertices,
                                                              const u8* vertex_data)
{
  // Skip if not indexed array
  if (!IsIndexed(array_type))
    return;

  u16 max_index = 0;

  // Determine min and max indices
  if (array_type == VertexComponentFormat::Index8)
  {
    for (u16 vertex_num = 0; vertex_num < num_vertices; vertex_num++)
    {
      const u8 index = vertex_data[component_offset];
      vertex_data += vertex_size;

      // 0xff skips the vertex
      if (index != 0xff)
      {
        if (index > max_index)
          max_index = index;
      }
    }
  }
  else
  {
    for (u16 vertex_num = 0; vertex_num < num_vertices; vertex_num++)
    {
      const u16 index = Common::swap16(&vertex_data[component_offset]);
      vertex_data += vertex_size;

      // 0xffff skips the vertex
      if (index != 0xffff)
      {
        if (index > max_index)
          max_index = index;
      }
    }
  }

  const u32 array_start = m_cpmem.array_bases[array_index];
  const u32 array_size = m_cpmem.array_strides[array_index] * (max_index + 1);

  m_owner->UseMemory(array_start, array_size, MemoryUpdate::VERTEX_STREAM);
}

static FifoRecorder instance;

FifoRecorder::FifoRecorder() = default;

void FifoRecorder::StartRecording(s32 numFrames, CallbackFunc finishedCb)
{
  std::lock_guard lk(m_mutex);

  m_File = std::make_unique<FifoDataFile>();

  // TODO: This, ideally, would be deallocated when done recording.
  //       However, care needs to be taken since global state
  //       and multithreading don't play well nicely together.
  //       The video thread may call into functions that utilize these
  //       despite 'end recording' being requested via StopRecording().
  //       (e.g. OpcodeDecoder calling UseMemory())
  //
  // Basically:
  //   - Singletons suck
  //   - Global variables suck
  //   - Multithreading with the above two sucks
  //
  m_Ram.resize(Memory::GetRamSize());
  m_ExRam.resize(Memory::GetExRamSize());

  std::fill(m_Ram.begin(), m_Ram.end(), 0);
  std::fill(m_ExRam.begin(), m_ExRam.end(), 0);

  m_File->SetIsWii(SConfig::GetInstance().bWii);

  if (!m_IsRecording)
  {
    m_WasRecording = false;
    m_IsRecording = true;
    m_RecordFramesRemaining = numFrames;
  }

  m_RequestedRecordingEnd = false;
  m_FinishedCb = finishedCb;
}

void FifoRecorder::StopRecording()
{
  std::lock_guard lk(m_mutex);
  m_RequestedRecordingEnd = true;
}

bool FifoRecorder::IsRecordingDone() const
{
  return m_WasRecording && m_File != nullptr;
}

FifoDataFile* FifoRecorder::GetRecordedFile() const
{
  return m_File.get();
}

void FifoRecorder::WriteGPCommand(const u8* data, u32 size)
{
  if (!m_SkipNextData)
  {
    // Assumes data contains all information for the command
    // Calls FifoRecorder::UseMemory
    const u32 analyzed_size = OpcodeDecoder::RunCommand(data, size, *m_record_analyzer);

    // Make sure FifoPlayer's command analyzer agrees about the size of the command.
    if (analyzed_size != size)
    {
      PanicAlertFmt("FifoRecorder: Expected command to be {} bytes long, we were given {} bytes",
                    analyzed_size, size);
    }

    // Copy data to buffer
    size_t currentSize = m_FifoData.size();
    m_FifoData.resize(currentSize + size);
    memcpy(&m_FifoData[currentSize], data, size);
  }

  if (m_FrameEnded && !m_FifoData.empty())
  {
    m_CurrentFrame.fifoData = m_FifoData;

    {
      std::lock_guard lk(m_mutex);

      // Copy frame to file
      // The file will be responsible for freeing the memory allocated for each frame's fifoData
      m_File->AddFrame(m_CurrentFrame);

      if (m_FinishedCb && m_RequestedRecordingEnd)
        m_FinishedCb();
    }

    m_CurrentFrame.memoryUpdates.clear();
    m_FifoData.clear();
    m_FrameEnded = false;
  }

  m_SkipNextData = m_SkipFutureData;
}

void FifoRecorder::UseMemory(u32 address, u32 size, MemoryUpdate::Type type, bool dynamicUpdate)
{
  u8* curData;
  u8* newData;
  if (address & 0x10000000)
  {
    curData = &m_ExRam[address & Memory::GetExRamMask()];
    newData = &Memory::m_pEXRAM[address & Memory::GetExRamMask()];
  }
  else
  {
    curData = &m_Ram[address & Memory::GetRamMask()];
    newData = &Memory::m_pRAM[address & Memory::GetRamMask()];
  }

  if (!dynamicUpdate && memcmp(curData, newData, size) != 0)
  {
    // Update current memory
    memcpy(curData, newData, size);

    // Record memory update
    MemoryUpdate memUpdate;
    memUpdate.address = address;
    memUpdate.fifoPosition = (u32)(m_FifoData.size());
    memUpdate.type = type;
    memUpdate.data.resize(size);
    std::copy(newData, newData + size, memUpdate.data.begin());

    m_CurrentFrame.memoryUpdates.push_back(std::move(memUpdate));
  }
  else if (dynamicUpdate)
  {
    // Shadow the data so it won't be recorded as changed by a future UseMemory
    memcpy(curData, newData, size);
  }
}

void FifoRecorder::EndFrame(u32 fifoStart, u32 fifoEnd)
{
  // m_IsRecording is assumed to be true at this point, otherwise this function would not be called
  std::lock_guard lk(m_mutex);

  m_FrameEnded = true;

  m_CurrentFrame.fifoStart = fifoStart;
  m_CurrentFrame.fifoEnd = fifoEnd;

  if (m_WasRecording)
  {
    // If recording a fixed number of frames then check if the end of the recording was reached
    if (m_RecordFramesRemaining > 0)
    {
      --m_RecordFramesRemaining;
      if (m_RecordFramesRemaining == 0)
        m_RequestedRecordingEnd = true;
    }
  }
  else
  {
    m_WasRecording = true;

    // Skip the first data which will be the frame copy command
    m_SkipNextData = true;
    m_SkipFutureData = false;

    m_FrameEnded = false;

    m_FifoData.reserve(1024 * 1024 * 4);
    m_FifoData.clear();
  }

  if (m_RequestedRecordingEnd)
  {
    // Skip data after the next time WriteFifoData is called
    m_SkipFutureData = true;
    // Signal video backend that it should not call this function when the next frame ends
    m_IsRecording = false;
  }
}

void FifoRecorder::SetVideoMemory(const u32* bpMem, const u32* cpMem, const u32* xfMem,
                                  const u32* xfRegs, u32 xfRegsSize, const u8* texMem)
{
  std::lock_guard lk(m_mutex);

  if (m_File)
  {
    memcpy(m_File->GetBPMem(), bpMem, FifoDataFile::BP_MEM_SIZE * 4);
    memcpy(m_File->GetCPMem(), cpMem, FifoDataFile::CP_MEM_SIZE * 4);
    memcpy(m_File->GetXFMem(), xfMem, FifoDataFile::XF_MEM_SIZE * 4);

    u32 xfRegsCopySize = std::min((u32)FifoDataFile::XF_REGS_SIZE, xfRegsSize);
    memcpy(m_File->GetXFRegs(), xfRegs, xfRegsCopySize * 4);

    memcpy(m_File->GetTexMem(), texMem, FifoDataFile::TEX_MEM_SIZE);
  }

  m_record_analyzer = std::make_unique<FifoRecordAnalyzer>(this, cpMem);
}

bool FifoRecorder::IsRecording() const
{
  return m_IsRecording;
}

FifoRecorder& FifoRecorder::GetInstance()
{
  return instance;
}
