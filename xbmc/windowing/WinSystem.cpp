/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystem.h"
#include "ServiceBroker.h"
#include "guilib/DispResource.h"
#include "powermanagement/DPMSSupport.h"
#include "windowing/GraphicContext.h"
#include "settings/DisplaySettings.h"
#include "settings/lib/Setting.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#if HAS_GLES
#include "guilib/GUIFontTTFGL.h"
#endif

const char* CWinSystemBase::SETTING_WINSYSTEM_IS_HDR_DISPLAY = "winsystem.ishdrdisplay";

CWinSystemBase::CWinSystemBase()
{
  m_gfxContext.reset(new CGraphicContext());
}

CWinSystemBase::~CWinSystemBase() = default;

bool CWinSystemBase::InitWindowSystem()
{
  UpdateResolutions();
  CDisplaySettings::GetInstance().ApplyCalibrations();
  return true;
}

bool CWinSystemBase::DestroyWindowSystem()
{
#if HAS_GLES
  CGUIFontTTFGL::DestroyStaticVertexBuffers();
#endif
  m_screenSaverManager.reset();
  return false;
}

void CWinSystemBase::UpdateDesktopResolution(RESOLUTION_INFO& newRes, const std::string &output, int width, int height, float refreshRate, uint32_t dwFlags)
{
  newRes.Overscan.left = 0;
  newRes.Overscan.top = 0;
  newRes.Overscan.right = width;
  newRes.Overscan.bottom = height;
  newRes.bFullScreen = true;
  newRes.iSubtitles = (int)(0.965 * height);
  newRes.dwFlags = dwFlags;
  newRes.fRefreshRate = refreshRate;
  newRes.fPixelRatio = 1.0f;
  newRes.iWidth = width;
  newRes.iHeight = height;
  newRes.iScreenWidth = width;
  newRes.iScreenHeight = height;
  newRes.strMode = StringUtils::Format("%s: %dx%d", output.c_str(), width, height);
  if (refreshRate > 1)
    newRes.strMode += StringUtils::Format(" @ %.2fHz", refreshRate);
  if (dwFlags & D3DPRESENTFLAG_INTERLACED)
    newRes.strMode += "i";
  if (dwFlags & D3DPRESENTFLAG_MODE3DTB)
    newRes.strMode += "tab";
  if (dwFlags & D3DPRESENTFLAG_MODE3DSBS)
    newRes.strMode += "sbs";
  newRes.strOutput = output;
}

void CWinSystemBase::UpdateResolutions()
{
  // add the window res - defaults are fine.
  RESOLUTION_INFO& window = CDisplaySettings::GetInstance().GetResolutionInfo(RES_WINDOW);
  window.bFullScreen = false;
  if (window.iWidth == 0)
    window.iWidth = 720;
  if (window.iHeight == 0)
    window.iHeight = 480;
  window.iScreenWidth  = window.iWidth;
  window.iScreenHeight = window.iHeight;
  if (window.iSubtitles == 0)
    window.iSubtitles = (int)(0.965 * window.iHeight);
  window.fPixelRatio = 1.0f;
  window.strMode = "Windowed";
}

void CWinSystemBase::SetWindowResolution(int width, int height)
{
  RESOLUTION_INFO& window = CDisplaySettings::GetInstance().GetResolutionInfo(RES_WINDOW);
  window.iWidth = width;
  window.iHeight = height;
  window.iScreenWidth = width;
  window.iScreenHeight = height;
  window.iSubtitles = (int)(0.965 * window.iHeight);
  CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(window);
}

static void AddResolution(std::vector<RESOLUTION_WHR> &resolutions, unsigned int addindex, float bestRefreshrate)
{
  RESOLUTION_INFO resInfo = CDisplaySettings::GetInstance().GetResolutionInfo(addindex);
  int width  = resInfo.iScreenWidth;
  int height = resInfo.iScreenHeight;
  int flags  = resInfo.dwFlags & D3DPRESENTFLAG_MODEMASK;
  float refreshrate = resInfo.fRefreshRate;

  // don't touch RES_DESKTOP
  for (unsigned int idx = 1; idx < resolutions.size(); idx++)
    if (   resolutions[idx].width == width
        && resolutions[idx].height == height
        &&(resolutions[idx].flags & D3DPRESENTFLAG_MODEMASK) == flags)
    {
      // check if the refresh rate of this resolution is better suited than
      // the refresh rate of the resolution with the same width/height/interlaced
      // property and if so replace it
      if (bestRefreshrate > 0.0 && refreshrate == bestRefreshrate)
        resolutions[idx].ResInfo_Index = addindex;

      // no need to add the resolution again
      return;
    }

  RESOLUTION_WHR res = {width, height, flags, (int)addindex};
  resolutions.push_back(res);
}

static bool resSortPredicate(RESOLUTION_WHR i, RESOLUTION_WHR j)
{
  // note: this comparison must obey "strict weak ordering"
  // a "!=" on the flags comparison resulted in memory corruption
  return (    i.width < j.width
          || (i.width == j.width && i.height < j.height)
          || (i.width == j.width && i.height == j.height && i.flags < j.flags) );
}

std::vector<RESOLUTION_WHR> CWinSystemBase::ScreenResolutions(float refreshrate)
{
  std::vector<RESOLUTION_WHR> resolutions;

  for (unsigned int idx = RES_CUSTOM; idx < CDisplaySettings::GetInstance().ResolutionInfoSize(); idx++)
  {
    RESOLUTION_INFO info = CDisplaySettings::GetInstance().GetResolutionInfo(idx);
    AddResolution(resolutions, idx, refreshrate);
  }

  // Can't assume a sort order
  sort(resolutions.begin(), resolutions.end(), resSortPredicate);

  return resolutions;
}

static void AddRefreshRate(std::vector<REFRESHRATE> &refreshrates, unsigned int addindex)
{
  float RefreshRate = CDisplaySettings::GetInstance().GetResolutionInfo(addindex).fRefreshRate;

  for (unsigned int idx = 0; idx < refreshrates.size(); idx++)
    if (   refreshrates[idx].RefreshRate == RefreshRate)
      return; // already taken care of.

  REFRESHRATE rr = {RefreshRate, (int)addindex};
  refreshrates.push_back(rr);
}

static bool rrSortPredicate(REFRESHRATE i, REFRESHRATE j)
{
  return (i.RefreshRate < j.RefreshRate);
}

std::vector<REFRESHRATE> CWinSystemBase::RefreshRates(int width, int height, uint32_t dwFlags)
{
  std::vector<REFRESHRATE> refreshrates;

  for (unsigned int idx = RES_DESKTOP; idx < CDisplaySettings::GetInstance().ResolutionInfoSize(); idx++)
  {
    if (CDisplaySettings::GetInstance().GetResolutionInfo(idx).iScreenWidth  == width &&
        CDisplaySettings::GetInstance().GetResolutionInfo(idx).iScreenHeight == height &&
        (CDisplaySettings::GetInstance().GetResolutionInfo(idx).dwFlags & D3DPRESENTFLAG_MODEMASK) == (dwFlags & D3DPRESENTFLAG_MODEMASK))
      AddRefreshRate(refreshrates, idx);
  }

  // Can't assume a sort order
  sort(refreshrates.begin(), refreshrates.end(), rrSortPredicate);

  return refreshrates;
}

REFRESHRATE CWinSystemBase::DefaultRefreshRate(std::vector<REFRESHRATE> rates)
{
  REFRESHRATE bestmatch = rates[0];
  float bestfitness = -1.0f;
  float targetfps = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP).fRefreshRate;

  for (unsigned i = 0; i < rates.size(); i++)
  {
    float fitness = fabs(targetfps - rates[i].RefreshRate);

    if (bestfitness <0 || fitness < bestfitness)
    {
      bestfitness = fitness;
      bestmatch = rates[i];
      if (bestfitness == 0.0f) // perfect match
        break;
    }
  }
  return bestmatch;
}

bool CWinSystemBase::UseLimitedColor()
{
  return false;
}

std::string CWinSystemBase::GetClipboardText(void)
{
  return "";
}

int CWinSystemBase::NoOfBuffers(void)
{
  int buffers = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOSCREEN_NOOFBUFFERS);
  return buffers;
}

KODI::WINDOWING::COSScreenSaverManager* CWinSystemBase::GetOSScreenSaver()
{
  if (!m_screenSaverManager)
  {
    auto impl = GetOSScreenSaverImpl();
    if (impl)
    {
      m_screenSaverManager.reset(new KODI::WINDOWING::COSScreenSaverManager(std::move(impl)));
    }
  }

  return m_screenSaverManager.get();
}

void CWinSystemBase::RegisterRenderLoop(IRenderLoop *client)
{
  CSingleLock lock(m_renderLoopSection);
  m_renderLoopClients.push_back(client);
}

void CWinSystemBase::UnregisterRenderLoop(IRenderLoop *client)
{
  CSingleLock lock(m_renderLoopSection);
  auto i = find(m_renderLoopClients.begin(), m_renderLoopClients.end(), client);
  if (i != m_renderLoopClients.end())
    m_renderLoopClients.erase(i);
}

void CWinSystemBase::DriveRenderLoop()
{
  MessagePump();

  { CSingleLock lock(m_renderLoopSection);
    for (auto i = m_renderLoopClients.begin(); i != m_renderLoopClients.end(); ++i)
      (*i)->FrameMove();
  }
}

CGraphicContext& CWinSystemBase::GetGfxContext()
{
  return *m_gfxContext;
}

std::shared_ptr<CDPMSSupport> CWinSystemBase::GetDPMSManager()
{
  return m_dpms;
}

void CWinSystemBase::ToggleWindowsHDR()
{
  uint32_t pathCount, modeCount;

  uint8_t set[] = {0x0A, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x14, 0x81, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};

  uint8_t request[] = {0x09, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x7C, 0x6F, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0xDB, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00};

  if (ERROR_SUCCESS == GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount))
  {
    DISPLAYCONFIG_PATH_INFO* pathsArray = nullptr;
    DISPLAYCONFIG_MODE_INFO* modesArray = nullptr;

    const size_t sizePathsArray = pathCount * sizeof(DISPLAYCONFIG_PATH_INFO);
    const size_t sizeModesArray = modeCount * sizeof(DISPLAYCONFIG_MODE_INFO);

    pathsArray = static_cast<DISPLAYCONFIG_PATH_INFO*>(std::malloc(sizePathsArray));
    modesArray = static_cast<DISPLAYCONFIG_MODE_INFO*>(std::malloc(sizeModesArray));

    if (pathsArray != nullptr && modesArray != nullptr)
    {
      std::memset(pathsArray, 0, sizePathsArray);
      std::memset(modesArray, 0, sizeModesArray);

      if (ERROR_SUCCESS == QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, pathsArray,
                                              &modeCount, modesArray, 0))
      {
        DISPLAYCONFIG_DEVICE_INFO_HEADER* setPacket =
            reinterpret_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER*>(set);
        DISPLAYCONFIG_DEVICE_INFO_HEADER* requestPacket =
            reinterpret_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER*>(request);

        for (int i = 0; i < modeCount; i++)
        {
          if (modesArray[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
          {
            setPacket->adapterId.HighPart = modesArray[i].adapterId.HighPart;
            setPacket->adapterId.LowPart = modesArray[i].adapterId.LowPart;
            setPacket->id = modesArray[i].id;

            requestPacket->adapterId.HighPart = modesArray[i].adapterId.HighPart;
            requestPacket->adapterId.LowPart = modesArray[i].adapterId.LowPart;
            requestPacket->id = modesArray[i].id;
          }
        }

        if (ERROR_SUCCESS == DisplayConfigGetDeviceInfo(requestPacket))
        {
          if (request[20] == 0xD1) // HDR is OFF
          {
            set[20] = 1;
            CLog::LogF(LOGNOTICE, "Toggle Windows HDR On (OFF => ON).");
            DisplayConfigSetDeviceInfo(setPacket);
          }
          else if (request[20] == 0xD3) // HDR is ON
          {
            set[20] = 0;
            CLog::LogF(LOGNOTICE, "Toggle Windows HDR Off (ON => OFF).");
            DisplayConfigSetDeviceInfo(setPacket);
          }
        }
      }
      std::free(pathsArray);
      std::free(modesArray);
    }
  }
}

int CWinSystemBase::GetHDRDisplayStatus()
{
  uint32_t pathCount, modeCount;

  uint8_t request[] = {0x09, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x7C, 0x6F, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0xDB, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00};

  int status = 0;

  if (ERROR_SUCCESS == GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount))
  {
    DISPLAYCONFIG_PATH_INFO* pathsArray = nullptr;
    DISPLAYCONFIG_MODE_INFO* modesArray = nullptr;

    const size_t sizePathsArray = pathCount * sizeof(DISPLAYCONFIG_PATH_INFO);
    const size_t sizeModesArray = modeCount * sizeof(DISPLAYCONFIG_MODE_INFO);

    pathsArray = static_cast<DISPLAYCONFIG_PATH_INFO*>(std::malloc(sizePathsArray));
    modesArray = static_cast<DISPLAYCONFIG_MODE_INFO*>(std::malloc(sizeModesArray));

    if (pathsArray != nullptr && modesArray != nullptr)
    {
      std::memset(pathsArray, 0, sizePathsArray);
      std::memset(modesArray, 0, sizeModesArray);

      if (ERROR_SUCCESS == QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, pathsArray,
                                              &modeCount, modesArray, 0))
      {
        DISPLAYCONFIG_DEVICE_INFO_HEADER* requestPacket =
            reinterpret_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER*>(request);

        for (int i = 0; i < modeCount; i++)
        {
          if (modesArray[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
          {
            requestPacket->adapterId.HighPart = modesArray[i].adapterId.HighPart;
            requestPacket->adapterId.LowPart = modesArray[i].adapterId.LowPart;
            requestPacket->id = modesArray[i].id;
          }
        }

        if (ERROR_SUCCESS == DisplayConfigGetDeviceInfo(requestPacket))
        {
          std::string txDeviceInfo;
          switch (request[20])
          {
            case 0xD0: // display is not HDR capable
              status = 0;
              txDeviceInfo = "No HDR capable";
              break;
            case 0xD1: // capable and HDR is OFF
              status = 1;
              txDeviceInfo = "HDR capable and OFF";
              break;
            case 0xD3: // capable and HDR is ON
              status = 2;
              txDeviceInfo = "HDR capable and ON";
              break;
            default:
              status = 0;
              txDeviceInfo = "UNKNOWN";
              break;
          }
          if (CServiceBroker::IsServiceManagerUp())
            CLog::LogF(LOGDEBUG,
                       "DisplayConfigGetDeviceInfo returned value 0x{0:2X} \"{1:s}\"  (return "
                       "status = {2:d})",
                       request[20], txDeviceInfo, status);
        }
      }
      std::free(pathsArray);
      std::free(modesArray);
    }
  }

  return status;
}

bool CWinSystemBase::IsDisplayHDREnabled()
{
  if (GetHDRDisplayStatus() == 2)
    return true;

  return false;
}
