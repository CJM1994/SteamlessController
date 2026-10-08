#include "DsuMotion.h"
#include "steam/SteamController.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// GamepadMotionHelpers' stillness tuning, as sc2dsu uses it.
constexpr uint32_t kMinStillnessSamples        = 10;
constexpr float    kMinStillnessCollectionTime = 0.5f;
constexpr float    kMinStillnessCorrectionTime = 2.0f;
constexpr float    kMaxStillnessError          = 2.0f;
constexpr float    kSampleDeteriorationRate    = 0.2f;
constexpr float    kErrorClimbRate             = 0.1f;
constexpr float    kErrorDropOnRecalibrate     = 0.1f;
constexpr float    kEaseInTime                 = 3.0f;
constexpr float    kHalfTime                   = 0.1f;
constexpr float    kConfidenceRate             = 1.0f;

float Apply(const DsuAxis& axis, const float raw[3]) {
    const float v = raw[(std::min)(axis.source, uint8_t(2))];
    return axis.invert ? -v : v;
}

int16_t I16(const uint8_t* p) {
    int16_t v;
    memcpy(&v, p, 2);
    return v;
}

uint8_t Bits(std::initializer_list<std::pair<bool, int>> bits) {
    uint8_t v = 0;
    for (auto [on, pos] : bits) if (on) v |= static_cast<uint8_t>(1u << pos);
    return v;
}

// A signed pad axis to a DualShock 4 touchpad coordinate, as the virtual
// PlayStation pad scales it (VirtualController's NormalizePadAxis).
uint16_t PadAxis(int16_t raw, int maxValue, bool invert) {
    int v = raw;
    if (invert) v = (v == -32768) ? 32767 : -v;
    const int scaled = static_cast<int>((static_cast<int64_t>(v) + 32768) * maxValue / 65535);
    return static_cast<uint16_t>((std::clamp)(scaled, 0, maxValue));
}

uint8_t Stick(int16_t v)   { return static_cast<uint8_t>((int32_t(v) + 32768) >> 8); }
uint8_t Trigger(int16_t v) { return static_cast<uint8_t>((std::min)((std::max)(v, int16_t(0)) >> 7, 255)); }

}  // namespace

// ---- Axis maps -------------------------------------------------------------

bool DsuAxisMap::Parse(const std::wstring& text, DsuAxisMap& out) {
    DsuAxis axes[3];
    int count = 0;
    size_t pos = 0;
    while (pos <= text.size() && count < 3) {
        size_t comma = text.find(L',', pos);
        if (comma == std::wstring::npos) comma = text.size();
        std::wstring part;
        for (size_t i = pos; i < comma; ++i) if (text[i] != L' ') part += towlower(text[i]);
        pos = comma + 1;
        bool invert = false;
        if (!part.empty() && part[0] == L'-') { invert = true; part.erase(0, 1); }
        if (part.size() != 1 || part[0] < L'x' || part[0] > L'z') return false;
        axes[count++] = { static_cast<uint8_t>(part[0] - L'x'), invert };
    }
    if (count != 3) return false;
    out = { axes[0], axes[1], axes[2] };
    return true;
}

std::wstring DsuAxisMap::ToString() const {
    std::wstring s;
    for (const DsuAxis* a : { &x, &y, &z }) {
        if (!s.empty()) s += L',';
        if (a->invert) s += L'-';
        s += static_cast<wchar_t>(L'x' + a->source);
    }
    return s;
}

// ---- Calibration -----------------------------------------------------------

void DsuGyroCalibration::Window::Add(const float gyro[3], const float accel[3], float dt) {
    if (samples == 0) {
        for (int i = 0; i < 3; ++i) {
            minGyro[i] = maxGyro[i] = meanGyro[i] = gyro[i];
            minAccel[i] = maxAccel[i] = meanAccel[i] = accel[i];
        }
        samples = 1;
        time    = dt;
        return;
    }
    ++samples;
    time += dt;
    const float inv = 1.0f / static_cast<float>(samples);
    for (int i = 0; i < 3; ++i) {
        minGyro[i]  = (std::min)(minGyro[i], gyro[i]);
        maxGyro[i]  = (std::max)(maxGyro[i], gyro[i]);
        minAccel[i] = (std::min)(minAccel[i], accel[i]);
        maxAccel[i] = (std::max)(maxAccel[i], accel[i]);
        meanGyro[i]  += (gyro[i] - meanGyro[i]) * inv;
        meanAccel[i] += (accel[i] - meanAccel[i]) * inv;
    }
}

void DsuGyroCalibration::Correct(float gyro[3], const float accel[3], float dt) {
    if (dt > 0) AddSample(gyro, accel, dt);
    for (int i = 0; i < 3; ++i) gyro[i] -= m_bias[i];
}

void DsuGyroCalibration::AddSample(const float gyro[3], const float accel[3], float dt) {
    if (gyro[0] == 0 && gyro[1] == 0 && gyro[2] == 0
            && accel[0] == 0 && accel[1] == 0 && accel[2] == 0)
        return;

    m_window.Add(gyro, accel, dt);
    float gyroDelta[3], accelDelta[3];
    for (int i = 0; i < 3; ++i) {
        gyroDelta[i]  = m_window.maxGyro[i] - m_window.minGyro[i];
        accelDelta[i] = m_window.maxAccel[i] - m_window.minAccel[i];
    }

    if (m_confidence < 1.0f) {
        const float climb = kSampleDeteriorationRate * dt;
        for (int i = 0; i < 3; ++i) {
            m_minDeltaGyro[i]  += climb;
            m_minDeltaAccel[i] += climb;
        }
    }

    auto climbThreshold = [&] {
        m_threshold = (std::min)(m_threshold + kErrorClimbRate * dt, kMaxStillnessError);
    };

    if (m_window.samples < kMinStillnessSamples || m_window.time < kMinStillnessCollectionTime) {
        climbThreshold();
        return;
    }

    for (int i = 0; i < 3; ++i) {
        m_minDeltaGyro[i]  = (std::min)(m_minDeltaGyro[i], gyroDelta[i]);
        m_minDeltaAccel[i] = (std::min)(m_minDeltaAccel[i], accelDelta[i]);
    }

    bool still = true;
    for (int i = 0; i < 3; ++i)
        still = still && gyroDelta[i] <= m_minDeltaGyro[i] * m_threshold
                      && accelDelta[i] <= m_minDeltaAccel[i] * m_threshold;

    if (still) {
        if (m_window.time < kMinStillnessCorrectionTime) {
            climbThreshold();
            m_steady = false;
            return;
        }
        m_timeSteady = (std::min)(m_timeSteady + dt, kEaseInTime);
        const float easeIn   = m_timeSteady / kEaseInTime;
        const float halfTime = kHalfTime * m_confidence;
        // Zero confidence snaps straight to the window mean on the first lock.
        const float lerp = halfTime <= 0 ? 0.0f : std::exp2(-easeIn * dt / halfTime);
        for (int i = 0; i < 3; ++i)
            m_bias[i] = m_window.meanGyro[i] + (m_bias[i] - m_window.meanGyro[i]) * lerp;
        m_confidence = (std::min)(m_confidence + dt * kConfidenceRate, 1.0f);
        m_steady     = true;
    } else if (m_timeSteady > 0) {
        m_threshold  = (std::max)(m_threshold - kErrorDropOnRecalibrate, 1.0f);
        m_timeSteady = 0;
        m_window     = Window{};
        m_steady     = false;
    } else {
        climbThreshold();
        m_window = Window{};
        m_steady = false;
    }
}

// ---- Samples ---------------------------------------------------------------

void DsuMotion::Recalibrate() {
    m_calibration.Reset();
    m_lastUs = 0;
}

bool DsuMotion::Build(const uint8_t* buf, size_t n, const DsuMotionConfig& cfg,
                      uint64_t nowUs, DsuSample& out) {
    // Motion is at 30..45; shorter means the sensors are not on yet.
    if (n < 46) return false;
    using SC = SteamController;

    // Raw units: ±2 g and ±2000 dps across the full int16 range.
    const float rawAccel[3] = { I16(buf + 34) / 32768.0f * 2.0f,
                                I16(buf + 36) / 32768.0f * 2.0f,
                                I16(buf + 38) / 32768.0f * 2.0f };
    const float rawGyro[3]  = { I16(buf + 40) / 32768.0f * 2000.0f,
                                I16(buf + 42) / 32768.0f * 2000.0f,
                                I16(buf + 44) / 32768.0f * 2000.0f };
    float accel[3] = { Apply(cfg.accel.x, rawAccel), Apply(cfg.accel.y, rawAccel),
                       Apply(cfg.accel.z, rawAccel) };
    float gyro[3]  = { Apply(cfg.gyro.x, rawGyro), Apply(cfg.gyro.y, rawGyro),
                       Apply(cfg.gyro.z, rawGyro) };

    // A gap over 100 ms means the stream was interrupted; treating it as one
    // long step would feed the calibration a lie.
    const float dt = m_lastUs ? (std::min)(float(nowUs - m_lastUs) / 1e6f, 0.1f) : 0.0f;
    m_lastUs = nowUs;
    if (cfg.autoCalibrate) m_calibration.Correct(gyro, accel, dt);
    const float sensitivity = std::isfinite(cfg.gyroSensitivity)
                                  ? (std::clamp)(cfg.gyroSensitivity, 0.1f, 3.0f) : 1.0f;
    for (float& g : gyro) g *= sensitivity;

    out.timestampUs = nowUs;
    memcpy(out.accelG, accel, sizeof(accel));
    memcpy(out.gyroDps, gyro, sizeof(gyro));

    // Raw buttons, not this app's remaps: an emulator taking buttons from DSU
    // sees the controller as it is. The virtual pad is where the remaps go.
    const uint8_t b2 = buf[2], b3 = buf[3], b4 = buf[4];
    const uint8_t l2 = Trigger(I16(buf + 6)), r2 = Trigger(I16(buf + 8));
    const bool up = b3 & SC::BTN_DPAD_UP, down = b3 & SC::BTN_DPAD_DN;
    const bool left = b3 & SC::BTN_DPAD_LT, right = b3 & SC::BTN_DPAD_RT;
    const bool a = b2 & SC::BTN_A, b = b2 & SC::BTN_B, x = b2 & SC::BTN_X, y = b2 & SC::BTN_Y;
    const bool lb = b4 & SC::BTN_LB, rb = b3 & SC::BTN_RB;
    out.buttons1 = Bits({ { (b3 & SC::BTN_VIEW) != 0, 0 }, { (b3 & SC::BTN_LS) != 0, 1 },
                          { (b2 & SC::BTN_RS) != 0, 2 },   { (b2 & SC::BTN_MENU) != 0, 3 },
                          { up, 4 }, { right, 5 }, { down, 6 }, { left, 7 } });
    out.buttons2 = Bits({ { l2 >= 200, 0 }, { r2 >= 200, 1 }, { lb, 2 }, { rb, 3 },
                          { x, 4 }, { a, 5 }, { b, 6 }, { y, 7 } });
    out.home        = (b4 & SC::BTN_STEAM) ? 1 : 0;
    out.touchButton = (b2 & 0x10) ? 1 : 0;  // the "..." button beside Steam
    out.leftX  = Stick(I16(buf + 10));
    out.leftY  = Stick(I16(buf + 12));
    out.rightX = Stick(I16(buf + 14));
    out.rightY = Stick(I16(buf + 16));
    const uint8_t full = 255;
    const uint8_t analog[12] = { left ? full : uint8_t(0), down ? full : uint8_t(0),
                                 right ? full : uint8_t(0), up ? full : uint8_t(0),
                                 y ? full : uint8_t(0), b ? full : uint8_t(0),
                                 a ? full : uint8_t(0), x ? full : uint8_t(0),
                                 rb ? full : uint8_t(0), lb ? full : uint8_t(0), r2, l2 };
    memcpy(out.analog, analog, sizeof(analog));

    // The right pad as the touch point — a 3DS touchscreen, in Azahar. Read
    // from the report itself, so it works whatever the pad is set to do on
    // the desktop: as a mouse it moves the cursor too, as None it does only
    // this. The "active" bit is contact, not a click.
    const bool touching = (b4 & SC::BTN_TP_RT) != 0;
    if (touching && !m_touching) ++m_touchId;
    m_touching = touching;
    out.touch[0].active = touching;
    out.touch[0].id     = m_touchId;
    if (touching) {
        out.touch[0].x = PadAxis(I16(buf + 24), 1919, false);
        out.touch[0].y = PadAxis(I16(buf + 26), 942, true);
    }
    return true;
}
