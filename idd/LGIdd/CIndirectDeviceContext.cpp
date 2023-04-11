#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

#include "CPlatformInfo.h"
#include "Debug.h"

#include <sstream>

static const struct LGMPQueueConfig FRAME_QUEUE_CONFIG =
{
  LGMP_Q_FRAME,       //queueID
  LGMP_Q_FRAME_LEN,   //numMessages
  1000                //subTimeout
};

static const struct LGMPQueueConfig POINTER_QUEUE_CONFIG =
{
  LGMP_Q_POINTER,     //queueID
  LGMP_Q_POINTER_LEN, //numMesages
  1000                //subTimeout
};

CIndirectDeviceContext::~CIndirectDeviceContext()
{
  if (m_lgmp == nullptr)
    return;

  if (m_lgmpTimer)
  {
    WdfTimerStop(m_lgmpTimer, TRUE);
    m_lgmpTimer = nullptr;
  }

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&m_frameMemory[i]);
  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&m_pointerMemory[i]);
  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerShapeMemory[i]);
  lgmpHostFree(&m_lgmp);
}

void CIndirectDeviceContext::InitAdapter()
{
  if (!SetupLGMP())
    return;

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);  

  caps.MaxMonitorsSupported = 1;

  caps.EndPointDiagnostics.Size             = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport     = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;

  caps.EndPointDiagnostics.pEndPointFriendlyName     = L"Looking Glass IDD Device";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"Looking Glass";
  caps.EndPointDiagnostics.pEndPointModelName        = L"Looking Glass";

  IDDCX_ENDPOINT_VERSION ver = {};
  ver.Size     = sizeof(ver);
  ver.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &ver;
  caps.EndPointDiagnostics.pHardwareVersion = &ver;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectDeviceContextWrapper);

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice        = m_wdfDevice;
  init.pCaps            = &caps;
  init.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT initOut;
  NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
  if (!NT_SUCCESS(status))
  {
    DBGPRINT("IddCxAdapterInitAsync Failed");
    return;
  }

  m_adapter = initOut.AdapterObject;

  // try to co-exist with the virtual video device by telling IddCx which adapter we prefer to render on
  IDXGIFactory * factory = NULL;
  IDXGIAdapter * dxgiAdapter;
  CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory);
  for (UINT i = 0; factory->EnumAdapters(i, &dxgiAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC adapterDesc;
    dxgiAdapter->GetDesc(&adapterDesc);
    dxgiAdapter->Release();

    if ((adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c) || // Microsoft Basic Render Driver
        (adapterDesc.VendorId == 0x1b36 && adapterDesc.DeviceId == 0x000d) || // QXL      
        (adapterDesc.VendorId == 0x1234 && adapterDesc.DeviceId == 0x1111))   // QEMU Standard VGA
      continue;

    IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
    args.PreferredRenderAdapter = adapterDesc.AdapterLuid;
    IddCxAdapterSetRenderAdapter(m_adapter, &args);
    break;
  }
  factory->Release();

  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(m_adapter);
  wrapper->context = this;
}

static const BYTE EDID[] =
{
  0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x30,0xE8,0x34,0x12,0xC9,0x07,0xCC,0x00,
  0x01,0x21,0x01,0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,
  0x0B,0x50,0x54,0x00,0x02,0x00,0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
  0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,
  0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,0x00,0x4C,0x6F,0x6F,
  0x6B,0x69,0x6E,0x67,0x47,0x6C,0x61,0x73,0x73,0x0A,0x00,0x00,0x00,0xFC,0x00,0x4C,
  0x6F,0x6F,0x6B,0x69,0x6E,0x67,0x20,0x47,0x6C,0x61,0x73,0x73,0x00,0x00,0x00,0xFD,
  0x00,0x28,0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x4A
};

void CIndirectDeviceContext::FinishInit(UINT connectorIndex)
{
  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectMonitorContextWrapper);

  IDDCX_MONITOR_INFO info = {};
  info.Size           = sizeof(info);
  info.MonitorType    = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  if (connectorIndex >= 1)
  {
    info.MonitorDescription.DataSize = 0;
    info.MonitorDescription.pData    = nullptr;
  }
  else
  {
    info.MonitorDescription.DataSize = sizeof(EDID);
    info.MonitorDescription.pData    = const_cast<BYTE*>(EDID);
  }

  CoCreateGuid(&info.MonitorContainerId);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut;
  NTSTATUS status = IddCxMonitorCreate(m_adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DBGPRINT("IddCxMonitorCreate Failed");
    return;
  }

  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(createOut.MonitorObject);
  wrapper->context = new CIndirectMonitorContext(createOut.MonitorObject, this);

  IDARG_OUT_MONITORARRIVAL out;
  status = IddCxMonitorArrival(createOut.MonitorObject, &out);
}

bool CIndirectDeviceContext::SetupLGMP()
{
  if (!m_ivshmem.Init() || !m_ivshmem.Open())
    return false;

  std::stringstream ss;
  {
    KVMFR kvmfr = {};
    memcpy_s(kvmfr.magic, sizeof(kvmfr.magic), KVMFR_MAGIC, sizeof(KVMFR_MAGIC) - 1);
    kvmfr.version  = KVMFR_VERSION;
    kvmfr.features = KVMFR_FEATURE_SETCURSORPOS;
    strncpy_s(kvmfr.hostver, "FIXME-IDD", sizeof(kvmfr.hostver) - 1);
    ss.write(reinterpret_cast<const char *>(&kvmfr), sizeof(kvmfr));
  }

  {
    const std::string & model = CPlatformInfo::GetCPUModel();

    KVMFRRecord_VMInfo * vmInfo = static_cast<KVMFRRecord_VMInfo *>(calloc(1, sizeof(*vmInfo)));
    if (!vmInfo)
    {
      DBGPRINT("Failed to allocate KVMFRRecord_VMInfo");
      return false;
    }
    vmInfo->cpus    = static_cast<uint8_t>(CPlatformInfo::GetProcCount  ());
    vmInfo->cores   = static_cast<uint8_t>(CPlatformInfo::GetCoreCount  ());
    vmInfo->sockets = static_cast<uint8_t>(CPlatformInfo::GetSocketCount());

    const uint8_t * uuid = CPlatformInfo::GetUUID();
    memcpy_s (vmInfo->uuid, sizeof(vmInfo->uuid), uuid, 16);
    strncpy_s(vmInfo->capture, "Idd Driver", sizeof(vmInfo->capture));

    KVMFRRecord * record = static_cast<KVMFRRecord *>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DBGPRINT("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_VMINFO;
    record->size = sizeof(*vmInfo) + (uint32_t)model.length() + 1;

    ss.write(reinterpret_cast<const char*>(record       ), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(vmInfo       ), sizeof(*vmInfo));
    ss.write(reinterpret_cast<const char*>(model.c_str()), model.length() + 1);
  }

  {
    KVMFRRecord_OSInfo * osInfo = static_cast<KVMFRRecord_OSInfo *>(calloc(1, sizeof(*osInfo)));
    if (!osInfo)
    {
      DBGPRINT("Failed to allocate KVMFRRecord_OSInfo");
      return false;
    }

    osInfo->os = KVMFR_OS_WINDOWS;

    const std::string & osName = CPlatformInfo::GetProductName();

    KVMFRRecord* record = static_cast<KVMFRRecord*>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DBGPRINT("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_OSINFO;
    record->size = sizeof(*osInfo) + (uint32_t)osName.length() + 1;

    ss.write(reinterpret_cast<const char*>(record), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(osInfo), sizeof(*osInfo));
    ss.write(reinterpret_cast<const char*>(osName.c_str()), osName.length() + 1);
  }

  LGMP_STATUS status;
  std::string udata = ss.str();

  if ((status = lgmpHostInit(m_ivshmem.GetMem(), (uint32_t)m_ivshmem.GetSize(),
    &m_lgmp, (uint32_t)udata.size(), (uint8_t*)&udata[0])) != LGMP_OK)
  {
    DBGPRINT("lgmpHostInit Failed: %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, FRAME_QUEUE_CONFIG, &m_frameQueue)) != LGMP_OK)
  {
    DBGPRINT("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, POINTER_QUEUE_CONFIG, &m_pointerQueue)) != LGMP_OK)
  {
    DBGPRINT("lgmpHostQueueCreate Failed (Pointer): %s", lgmpStatusString(status));
    return false;
  }

  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerMemory[i])) != LGMP_OK)
    {
      DBGPRINT("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerShapeMemory[i])) != LGMP_OK)
    {
      DBGPRINT("lgmpHostMemAlloc Failed (Pointer Shapes): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerShapeMemory[i]), 0, MAX_POINTER_SIZE);
  }

  m_maxFrameSize = lgmpHostMemAvail(m_lgmp);
  m_maxFrameSize = (m_maxFrameSize -(CPlatformInfo::GetPageSize() - 1)) & ~(CPlatformInfo::GetPageSize() - 1);
  m_maxFrameSize /= LGMP_Q_FRAME_LEN;
  DBGPRINT("Max Frame Size: %u MiB\n", (unsigned int)(m_maxFrameSize / 1048576LL));

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if ((status = lgmpHostMemAllocAligned(m_lgmp, (uint32_t)m_maxFrameSize,
        (uint32_t)CPlatformInfo::GetPageSize(), &m_frameMemory[i])) != LGMP_OK)
    {
      DBGPRINT("lgmpHostMemAllocAligned Failed (Frame): %s", lgmpStatusString(status));
      return false;
    }

  WDF_TIMER_CONFIG config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
    [](WDFTIMER timer) -> void
    {
      WDFOBJECT parent = WdfTimerGetParentObject(timer);
      auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(parent);
      wrapper->context->LGMPTimer();
    },
    10);
  config.AutomaticSerialization = FALSE;

  /**
  * documentation states that Dispatch is not available under the UDMF, however...
  * using Passive returns a not supported error, and Dispatch works.
  */
  WDF_OBJECT_ATTRIBUTES attribs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
  attribs.ParentObject   = m_wdfDevice;
  attribs.ExecutionLevel = WdfExecutionLevelDispatch;

  NTSTATUS s = WdfTimerCreate(&config, &attribs, &m_lgmpTimer);
  if (!NT_SUCCESS(s))
  {
    DBGPRINT("Timer creation failed: 0x%08x", s);
    return false;
  }
  WdfTimerStart(m_lgmpTimer, WDF_REL_TIMEOUT_IN_MS(10));

  return true;
}

void CIndirectDeviceContext::LGMPTimer()
{
  LGMP_STATUS status;
  if ((status = lgmpHostProcess(m_lgmp)) != LGMP_OK)
  {
    if (status == LGMP_ERR_CORRUPTED)
    {
      DBGPRINT("LGMP reported the shared memory has been corrupted, attempting to recover\n");
      //TODO: fixme - reinit
      return;
    }

    DBGPRINT("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    //TODO: fixme - shutdown
    return;
  }

  uint8_t data[LGMP_MSGS_SIZE];
  size_t  size;
  while ((status = lgmpHostReadData(m_pointerQueue, &data, &size)) == LGMP_OK)
  {
    KVMFRMessage * msg = (KVMFRMessage *)data;
    switch (msg->type)
    {
      case KVMFR_MESSAGE_SETCURSORPOS:
      {
        KVMFRSetCursorPos *sp = (KVMFRSetCursorPos *)msg;
        SetCursorPos(sp->x, sp->y);
        break;
      }
    }

    lgmpHostAckData(m_pointerQueue);
  }
}

void CIndirectDeviceContext::SendFrame(int width, int height, int pitch, DXGI_FORMAT format, void* data)
{
  if (!m_lgmp || !m_frameQueue)
    return;

  if (m_width != width || m_height != height || m_format != format)
  {
    m_width  = width;
    m_height = height;
    m_format = format;
    ++m_formatVer;
  }

  while (lgmpHostQueuePending(m_frameQueue) == LGMP_Q_FRAME_LEN)
    Sleep(0);

  if (++m_frameIndex == LGMP_Q_FRAME_LEN)
    m_frameIndex = 0;

  KVMFRFrame * fi = (KVMFRFrame *)lgmpHostMemPtr(m_frameMemory[m_frameIndex]);
  int bpp = 4;
  switch (format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM    : fi->type = FRAME_TYPE_BGRA   ; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM    : fi->type = FRAME_TYPE_RGBA   ; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM : fi->type = FRAME_TYPE_RGBA10 ; break;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      fi->type = FRAME_TYPE_RGBA16F;
      bpp = 8;
      break;

    default:
      DBGPRINT("Unsuppoted DXGI format");
      return;
  }

  //FIXME: this should not really be done here, this is a hack
  #pragma warning(push)
  #pragma warning(disable: 4200)
  struct FrameBuffer
  {
    volatile uint32_t wp;
    uint8_t data[0];
  };
  #pragma warning(pop)

  fi->formatVer    = m_formatVer;
  fi->frameSerial  = m_frameSerial++;
  fi->screenWidth  = width;
  fi->screenHeight = height;
  fi->frameWidth   = width;
  fi->frameHeight  = height;
  fi->stride       = width * bpp;
  fi->pitch        = pitch;
  fi->offset       = (uint32_t)(CPlatformInfo::GetPageSize() - sizeof(FrameBuffer));
  fi->flags        = 0;
  fi->rotation     = FRAME_ROT_0;

  fi->damageRectsCount = 0;

  FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)fi) + fi->offset);
  fb->wp = 0;

  lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[m_frameIndex]);
  memcpy(fb->data, data, (size_t)height * (size_t)pitch);
  fb->wp = height * pitch;
}