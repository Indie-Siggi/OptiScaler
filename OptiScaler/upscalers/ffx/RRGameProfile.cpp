#include <pch.h>
#include "RRGameProfile.h"

#include <Config.h>
#include "ffx_denoiser.h" // FFX_DENOISER_MODE_1_SIGNAL

RRGameProfile ResolveRRProfile()
{
    const Config& cfg = *Config::Instance();

    RRGameProfile p;
    p.name = "default";
    p.denoiserMode = FFX_DENOISER_MODE_1_SIGNAL;
    p.depthLinA = cfg.RrDepthLinA.value_or_default();
    p.depthLinB = cfg.RrDepthLinB.value_or_default();
    p.motionScaleX = cfg.RrMotionScaleX.value_or_default();
    p.motionScaleY = cfg.RrMotionScaleY.value_or_default();
    p.normalsArePacked = cfg.RrNormalsArePacked.value_or_default();
    p.demodulateRadiance = cfg.RrDemodulateRadiance.value_or_default();
    p.reversedZ = cfg.RrReversedZ.value_or_default();
    p.skyThreshold = cfg.RrSkyThreshold.value_or_default();
    p.debugView = static_cast<uint32_t>(cfg.RrDebugView.value_or_default());
    p.debugLog = cfg.RrDebugLog.value_or_default();

    // TODO(Step 5): switch on State::Instance().GameName / NVNGX_Engine to pick a per-game profile
    // (e.g. a Cyberpunk profile), then let the [RayRegen] ini values override individual fields.

    return p;
}
