// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"

namespace pimax_openxr {

    using namespace pimax_openxr::log;
    using namespace pimax_openxr::utils;
    using namespace xr::math;

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
    XrResult OpenXrRuntime::xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
        if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystem",
                          TLXArg(instance, "Instance"),
                          TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        }

        pvrHmdStatus status{};
        bool isStatusValid = false;

        // Workaround for PVR Home race condition upon destroying a session while an app (X-Plane 12) might be polling
        // the XrSystem.
        if (m_pvrSession) {
            CHECK_PVRCMD(pvr_getHmdStatus(m_pvrSession, &status));
            TraceLoggingWrite(g_traceProvider,
                              "PVR_HmdStatus",
                              TLArg(!!status.ServiceReady, "ServiceReady"),
                              TLArg(!!status.HmdPresent, "HmdPresent"),
                              TLArg(!!status.HmdMounted, "HmdMounted"),
                              TLArg(!!status.IsVisible, "IsVisible"),
                              TLArg(!!status.DisplayLost, "DisplayLost"),
                              TLArg(!!status.ShouldQuit, "ShouldQuit"));

            // If PVR Home took over the active session, then force re-creating our session in order to continue.
            if (status.ShouldQuit) {
                pvr_destroySession(m_pvrSession);
                m_pvrSession = nullptr;
            } else {
                isStatusValid = true;
            }
        }

        // Create the PVR session if needed.
        if (!ensurePvrSession()) {
            m_cachedHmdInfo = {};
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        }

        // Check for HMD presence.
        if (!isStatusValid) {
            CHECK_PVRCMD(pvr_getHmdStatus(m_pvrSession, &status));
            TraceLoggingWrite(g_traceProvider,
                              "PVR_HmdStatus",
                              TLArg(!!status.ServiceReady, "ServiceReady"),
                              TLArg(!!status.HmdPresent, "HmdPresent"),
                              TLArg(!!status.HmdMounted, "HmdMounted"),
                              TLArg(!!status.IsVisible, "IsVisible"),
                              TLArg(!!status.DisplayLost, "DisplayLost"),
                              TLArg(!!status.ShouldQuit, "ShouldQuit"));
        }
        if (!(status.ServiceReady && status.HmdPresent)) {
            m_cachedHmdInfo = {};
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        }

        // Query HMD properties.
        pvrHmdInfo hmdInfo{};
        CHECK_PVRCMD(pvr_getHmdInfo(m_pvrSession, &hmdInfo));
        TraceLoggingWrite(g_traceProvider,
                          "PVR_HmdInfo",
                          TLArg(hmdInfo.VendorId, "VendorId"),
                          TLArg(hmdInfo.ProductId, "ProductId"),
                          TLArg(hmdInfo.Manufacturer, "Manufacturer"),
                          TLArg(hmdInfo.ProductName, "ProductName"),
                          TLArg(hmdInfo.SerialNumber, "SerialNumber"),
                          TLArg(hmdInfo.FirmwareMinor, "FirmwareMinor"),
                          TLArg(hmdInfo.FirmwareMajor, "FirmwareMajor"),
                          TLArg(hmdInfo.Resolution.w, "ResolutionWidth"),
                          TLArg(hmdInfo.Resolution.h, "ResolutionHeight"));

        // Detect if the device changed.
        if (std::string_view(m_cachedHmdInfo.SerialNumber) != hmdInfo.SerialNumber) {
            m_cachedHmdInfo = hmdInfo;
            Log("Device is: %s\n", m_cachedHmdInfo.ProductName);

            // Important: anything below that set some state into the PVR session must be duplicated to
            // ensurePvrSession().

            // Ensure there is no stale parallel projection settings.
            CHECK_PVRCMD(pvr_setIntConfig(m_pvrSession, "view_rotation_fix", 0));

            // Check that we have consent to share eye gaze data with applications.
            m_isEyeTrackingAvailable = getSetting("allow_eye_tracking").value_or(false);

            // Detect eye tracker. This can take a while, so only do it when the app is requesting it.
            m_eyeTrackingType = EyeTracking::None;
            if (has_XR_EXT_eye_gaze_interaction) {
                if (getSetting("debug_eye_tracker").value_or(false)) {
                    m_eyeTrackingType = EyeTracking::Simulated;
                } else if (m_cachedHmdInfo.VendorId == 0x34A4 && m_cachedHmdInfo.ProductId == 0x0012) {
                    // Pimax Crystal uses the PVR SDK.
                    m_eyeTrackingType = EyeTracking::PVR;
                }
#ifndef NOASEEVRCLIENT
                else if (initializeDroolon()) {
                    // Other Pimax headsets use the 7invensun SDK (aSeeVR).
                    m_eyeTrackingType = EyeTracking::aSeeVR;
                }
#endif
            }
            if (m_eyeTrackingType == EyeTracking::None) {
                m_isEyeTrackingAvailable = false;
            }

            // Cache common information.
            CHECK_PVRCMD(pvr_getEyeRenderInfo(m_pvrSession, pvrEye_Left, &m_cachedEyeInfo[xr::StereoView::Left]));
            CHECK_PVRCMD(pvr_getEyeRenderInfo(m_pvrSession, pvrEye_Right, &m_cachedEyeInfo[xr::StereoView::Right]));

            m_floorHeight = pvr_getFloatConfig(m_pvrSession, CONFIG_KEY_EYE_HEIGHT, 0.f);
            TraceLoggingWrite(g_traceProvider,
                              "PVR_GetConfig",
                              TLArg(CONFIG_KEY_EYE_HEIGHT, "Config"),
                              TLArg(m_floorHeight, "EyeHeight"));

            const float cantingAngle = PVR::Quatf{m_cachedEyeInfo[xr::StereoView::Left].HmdToEyePose.Orientation}.Angle(
                                           m_cachedEyeInfo[xr::StereoView::Right].HmdToEyePose.Orientation) /
                                       2.f;
            m_useParallelProjection =
                cantingAngle > 0.0001f && getSetting("force_parallel_projection_state")
                                              .value_or(!pvr_getIntConfig(m_pvrSession, "steamvr_use_native_fov", 0));
            if (m_useParallelProjection) {
                Log("Parallel projection is enabled\n");

                // Per Pimax, we must set this value for parallel projection to work properly.
                CHECK_PVRCMD(pvr_setIntConfig(m_pvrSession, "view_rotation_fix", 1));

                // Update cached eye info to account for parallel projection.
                CHECK_PVRCMD(pvr_getEyeRenderInfo(m_pvrSession, pvrEye_Left, &m_cachedEyeInfo[xr::StereoView::Left]));
                CHECK_PVRCMD(pvr_getEyeRenderInfo(m_pvrSession, pvrEye_Right, &m_cachedEyeInfo[xr::StereoView::Right]));
            }
            m_fovLevel = pvr_getIntConfig(m_pvrSession, "fov_level", 0);

            for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
                m_cachedEyeFov[i].angleDown = -atan(m_cachedEyeInfo[i].Fov.DownTan);
                m_cachedEyeFov[i].angleUp = atan(m_cachedEyeInfo[i].Fov.UpTan);
                m_cachedEyeFov[i].angleLeft = -atan(m_cachedEyeInfo[i].Fov.LeftTan);
                m_cachedEyeFov[i].angleRight = atan(m_cachedEyeInfo[i].Fov.RightTan);

                TraceLoggingWrite(g_traceProvider,
                                  "PVR_EyeRenderInfo",
                                  TLArg(i == xr::StereoView::Left ? "Left" : "Right", "Eye"),
                                  TLArg(xr::ToString(m_cachedEyeInfo[i].HmdToEyePose).c_str(), "EyePose"),
                                  TLArg(xr::ToString(m_cachedEyeFov[i]).c_str(), "Fov"),
                                  TLArg(i == xr::StereoView::Left ? -cantingAngle : cantingAngle, "Canting"));
            }

            // Setup common parameters.
            CHECK_PVRCMD(pvr_setTrackingOriginType(m_pvrSession, pvrTrackingOrigin_EyeLevel));
        }

        m_systemCreated = true;
        *systemId = (XrSystemId)1;

        TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystemProperties
    XrResult OpenXrRuntime::xrGetSystemProperties(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrSystemProperties* properties) {
        if (properties->type != XR_TYPE_SYSTEM_PROPERTIES) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(
            g_traceProvider, "xrGetSystemProperties", TLXArg(instance, "Instance"), TLArg((int)systemId, "SystemId"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        XrSystemHandTrackingPropertiesEXT* handTrackingProperties =
            reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(properties->next);
        while (handTrackingProperties) {
            if (handTrackingProperties->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT) {
                break;
            }
            handTrackingProperties = reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(handTrackingProperties->next);
        }

        XrSystemEyeGazeInteractionPropertiesEXT* eyeGazeInteractionProperties =
            reinterpret_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(properties->next);
        while (eyeGazeInteractionProperties) {
            if (eyeGazeInteractionProperties->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) {
                break;
            }
            eyeGazeInteractionProperties =
                reinterpret_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(eyeGazeInteractionProperties->next);
        }

        properties->vendorId = m_cachedHmdInfo.VendorId;

        sprintf_s(properties->systemName, sizeof(properties->systemName), "%s", m_cachedHmdInfo.ProductName);
        properties->systemId = systemId;

        properties->trackingProperties.positionTracking = XR_TRUE;
        properties->trackingProperties.orientationTracking = XR_TRUE;

        static_assert(pvrMaxLayerCount >= XR_MIN_COMPOSITION_LAYERS_SUPPORTED);
        properties->graphicsProperties.maxLayerCount = pvrMaxLayerCount;
        properties->graphicsProperties.maxSwapchainImageWidth = 16384;
        properties->graphicsProperties.maxSwapchainImageHeight = 16384;

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystemProperties",
                          TLArg((int)properties->systemId, "SystemId"),
                          TLArg(properties->vendorId, "VendorId"),
                          TLArg(properties->systemName, "SystemName"),
                          TLArg(!!properties->trackingProperties.positionTracking, "PositionTracking"),
                          TLArg(!!properties->trackingProperties.orientationTracking, "OrientationTracking"),
                          TLArg(properties->graphicsProperties.maxLayerCount, "MaxLayerCount"),
                          TLArg(properties->graphicsProperties.maxSwapchainImageWidth, "MaxSwapchainImageWidth"),
                          TLArg(properties->graphicsProperties.maxSwapchainImageHeight, "MaxSwapchainImageHeight"));

        if (has_XR_EXT_hand_tracking && handTrackingProperties) {
            handTrackingProperties->supportsHandTracking = XR_TRUE;

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLArg((int)properties->systemId, "SystemId"),
                              TLArg(!!handTrackingProperties->supportsHandTracking, "SupportsHandTracking"));
        }

        if (has_XR_EXT_eye_gaze_interaction && eyeGazeInteractionProperties) {
            eyeGazeInteractionProperties->supportsEyeGazeInteraction = m_isEyeTrackingAvailable ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(
                g_traceProvider,
                "xrGetSystemProperties",
                TLArg((int)properties->systemId, "SystemId"),
                TLArg(!!eyeGazeInteractionProperties->supportsEyeGazeInteraction, "SupportsEyeGazeInteraction"));
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateEnvironmentBlendModes
    XrResult OpenXrRuntime::xrEnumerateEnvironmentBlendModes(XrInstance instance,
                                                             XrSystemId systemId,
                                                             XrViewConfigurationType viewConfigurationType,
                                                             uint32_t environmentBlendModeCapacityInput,
                                                             uint32_t* environmentBlendModeCountOutput,
                                                             XrEnvironmentBlendMode* environmentBlendModes) {
        // We only support immersive VR mode.
        static const XrEnvironmentBlendMode blendModes[] = {XR_ENVIRONMENT_BLEND_MODE_OPAQUE};

        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateEnvironmentBlendModes",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                          TLArg(environmentBlendModeCapacityInput, "EnvironmentBlendModeCapacityInput"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        }

        if (environmentBlendModeCapacityInput && environmentBlendModeCapacityInput < ARRAYSIZE(blendModes)) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *environmentBlendModeCountOutput = ARRAYSIZE(blendModes);
        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateEnvironmentBlendModes",
                          TLArg(*environmentBlendModeCountOutput, "EnvironmentBlendModeCountOutput"));

        if (environmentBlendModeCapacityInput && environmentBlendModes) {
            for (uint32_t i = 0; i < *environmentBlendModeCountOutput; i++) {
                environmentBlendModes[i] = blendModes[i];
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateEnvironmentBlendModes",
                                  TLArg(xr::ToCString(environmentBlendModes[i]), "EnvironmentBlendMode"));
            }
        }

        return XR_SUCCESS;
    }

    // Retrieve some information from PVR needed for graphic/frame management.
    void OpenXrRuntime::fillDisplayDeviceInfo() {
        CHECK_MSG(ensurePvrSession(), "PVR session was lost");

        pvrDisplayInfo info{};
        CHECK_PVRCMD(pvr_getEyeDisplayInfo(m_pvrSession, pvrEye_Left, &info));
        TraceLoggingWrite(g_traceProvider,
                          "PVR_EyeDisplayInfo",
                          TraceLoggingCharArray((char*)&info.luid, sizeof(LUID), "Luid"),
                          TLArg(info.edid_vid, "EdidVid"),
                          TLArg(info.edid_pid, "EdidPid"),
                          TLArg(info.pos_x, "PosX"),
                          TLArg(info.pos_y, "PosY"),
                          TLArg(info.width, "Width"),
                          TLArg(info.height, "Height"),
                          TLArg(info.refresh_rate, "RefreshRate"),
                          TLArg((int)info.disp_state, "DispState"),
                          TLArg((int)info.eye_display, "EyeDisplay"),
                          TLArg((int)info.eye_rotate, "EyeRotate"));

        // We also store the expected frame duration.
        m_displayRefreshRate = info.refresh_rate;
        m_idealFrameDuration = m_predictedFrameDuration = 1.0 / info.refresh_rate;

        memcpy(&m_adapterLuid, &info.luid, sizeof(LUID));
    }

    bool OpenXrRuntime::ensurePvrSession() {
        if (!m_pvrSession) {
            const auto result = pvr_createSession(m_pvr, &m_pvrSession);

            // This is the error returned when pi_server is not running. We pretend the HMD is not found.
            if (result == pvrResult::pvr_rpc_failed) {
                return false;
            }

            CHECK_PVRCMD(result);
            CHECK_PVRCMD(pvr_setIntConfig(m_pvrSession, "view_rotation_fix", m_useParallelProjection));
            CHECK_PVRCMD(pvr_setTrackingOriginType(m_pvrSession, pvrTrackingOrigin_EyeLevel));
        }

        return true;
    }

} // namespace pimax_openxr
