#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "DsuServer.h"

// Turning one Steam Controller 2026 state report into a DSU sample: units,
// axis mapping, drift calibration and sensitivity. Ported from sc2dsu's
// triton.rs, config.rs and gyro_calibration.rs (MIT); the calibration itself is
// sc2dsu's port of JibbSmart's GamepadMotionHelpers (MIT).

// Which raw axis feeds an output axis, and whether it is flipped.
struct DsuAxis {
    uint8_t source = 0;  // 0 = x, 1 = y, 2 = z
    bool    invert = false;
};
struct DsuAxisMap {
    DsuAxis x, y, z;
    // "x,-z,y": each output axis in turn, from a raw axis, '-' to invert.
    static bool Parse(const std::wstring& text, DsuAxisMap& out);
    std::wstring ToString() const;
};

struct DsuMotionConfig {
    // Matched to how Eden/Yuzu read Cemuhook UDP — (pitch, roll, -yaw) — which
    // is what sc2dsu settled on and what the other emulators also accept.
    DsuAxisMap gyro  { {0, false}, {2, true}, {1, false} };
    DsuAxisMap accel { {0, true},  {2, true}, {1, false} };
    float      gyroSensitivity = 1.0f;  // clamped to 0.1 .. 3.0
    bool       autoCalibrate   = true;
};

// Continuous gyro bias estimation: while the controller is judged to be lying
// still (gyro and accel barely moving across a growing window), the bias eases
// toward the window's mean gyro.
class DsuGyroCalibration {
public:
    // Corrected gyro for one sample; dt is seconds since the previous one.
    void Correct(float gyro[3], const float accel[3], float dt);
    void Reset() { *this = DsuGyroCalibration(); }
    bool Steady() const { return m_steady; }

private:
    struct Window {
        float minGyro[3], maxGyro[3], meanGyro[3];
        float minAccel[3], maxAccel[3], meanAccel[3];
        uint32_t samples = 0;
        float    time    = 0;
        void Add(const float gyro[3], const float accel[3], float dt);
    };
    void AddSample(const float gyro[3], const float accel[3], float dt);

    float  m_bias[3]          = {};
    float  m_confidence       = 0;
    float  m_timeSteady       = 0;
    float  m_minDeltaGyro[3]  = { 1.0f, 1.0f, 1.0f };
    float  m_minDeltaAccel[3] = { 0.25f, 0.25f, 0.25f };
    float  m_threshold        = 1.0f;
    bool   m_steady           = false;
    Window m_window;
};

// One per controller, owned by its read thread.
class DsuMotion {
public:
    // False for a report too short to carry motion (sensors still off).
    // nowUs is a monotonic host clock; the controller's own IMU timestamp is
    // not relied on, since what its units are is not settled.
    bool Build(const uint8_t* buf, size_t n, const DsuMotionConfig& cfg, uint64_t nowUs,
               DsuSample& out);
    void Recalibrate();

private:
    DsuGyroCalibration m_calibration;
    uint64_t           m_lastUs = 0;
    // The right pad as touch point 0: whether a finger was on it last frame,
    // and the id the current contact goes by.
    bool               m_touching = false;
    uint8_t            m_touchId  = 0;
};
