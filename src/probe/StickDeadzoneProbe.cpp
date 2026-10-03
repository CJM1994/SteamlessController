// Console diagnostic for the stick deadzone (#117): that rest reads as nothing,
// that the edge of the deadzone is continuous rather than a step, that full
// deflection still reaches full, and that a direction is left alone. None of it
// needs a controller.
//
// The size of the deadzone comes from StickRestProbe's measurements of real
// hardware; this checks that the maths honours whatever size it is given, which
// an off-by-one at the edge or a lost corner would get quietly wrong.
#include "app/StickConfig.h"
#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

double Len(StickPos p) {
    return std::hypot(static_cast<double>(p.x), static_cast<double>(p.y));
}

}  // namespace

int main() {
    const int dz = kStickDeadzoneRaw;

    printf("Inside the deadzone\n");
    {
        const StickPos zero = ApplyStickDeadzone(0, 0);
        Check(zero.x == 0 && zero.y == 0, "exact centre is nothing");

        const int16_t small = static_cast<int16_t>(dz / 2);
        const StickPos a = ApplyStickDeadzone(small, small);
        Check(a.x == 0 && a.y == 0, "a small diagonal drift is nothing");
        const StickPos b = ApplyStickDeadzone(static_cast<int16_t>(-small), small);
        Check(b.x == 0 && b.y == 0, "and so is one leaning the other way");

        // Radial, not per-axis: this point has each axis inside the deadzone but
        // its length well outside it, so a per-axis deadzone would zero it.
        const int16_t each = static_cast<int16_t>(dz * 0.9);
        const StickPos c = ApplyStickDeadzone(each, each);
        Check(c.x != 0 && c.y != 0, "a diagonal outside the circle is not cut per axis");

        const StickPos at = ApplyStickDeadzone(static_cast<int16_t>(dz), 0);
        Check(at.x == 0 && at.y == 0, "exactly on the edge is still nothing");
    }

    printf("\nWhat real sticks rest at\n");
    {
        // From StickRestProbe, hands off. The left stick's measured X and Y
        // ranges reach -587 and 1865, so their corner is a position harsher
        // than any single report it produced (the largest was radius 1923).
        const StickPos worst = ApplyStickDeadzone(-587, 1865);   // radius 1955, 5.97%
        Check(worst.x == 0 && worst.y == 0, "the worst measured resting position reads as nothing");
        const StickPos right = ApplyStickDeadzone(658, 111);     // radius 667
        Check(right.x == 0 && right.y == 0, "so does the other stick's");
        Check(Len(StickPos{ -587, 1865 }) < dz, "with room to spare, not by a hair");
        Check(dz >= static_cast<int>(1.3 * 1955), "at least 30% more than the worst rest seen");
    }

    printf("\nJust outside it\n");
    {
        const StickPos p = ApplyStickDeadzone(static_cast<int16_t>(dz + 1), 0);
        Check(p.x >= 0 && p.x <= 5, "one past the edge starts from near zero, not a step");
        Check(Len(ApplyStickDeadzone(static_cast<int16_t>(dz + 400), 0))
                  > Len(ApplyStickDeadzone(static_cast<int16_t>(dz + 200), 0)),
              "and grows with the stick");
    }

    printf("\nFull deflection\n");
    {
        const StickPos right = ApplyStickDeadzone(32767, 0);
        Check(right.x == 32767 && right.y == 0, "full right is still full right");
        const StickPos left = ApplyStickDeadzone(-32768, 0);
        Check(left.x == -32768 && left.y == 0, "full left reaches the bottom of the range");
        const StickPos up = ApplyStickDeadzone(0, 32767);
        Check(up.x == 0 && up.y == 32767, "full up is still full up");
        const StickPos down = ApplyStickDeadzone(0, -32768);
        Check(down.x == 0 && down.y == -32768, "full down reaches the bottom of the range");

        // A square gate's corner has a length past a circle's edge. It must not
        // come out smaller than it went in.
        const StickPos corner = ApplyStickDeadzone(32767, 32767);
        Check(corner.x == 32767 && corner.y == 32767, "a pushed-in corner still reaches full on both axes");
        const StickPos negCorner = ApplyStickDeadzone(-32768, -32768);
        Check(negCorner.x == -32768 && negCorner.y == -32768, "and so does the opposite corner");
    }

    printf("\nDirection\n");
    {
        // 30 degrees, at 60% of full. The angle must survive the rescale.
        const double mag = 0.6 * kStickRawMax;
        const double ang = 30.0 * 3.14159265358979323846 / 180.0;
        const StickPos p = ApplyStickDeadzone(static_cast<int16_t>(mag * std::cos(ang)),
                                              static_cast<int16_t>(mag * std::sin(ang)));
        const double outAng = std::atan2(static_cast<double>(p.y), static_cast<double>(p.x))
                            * 180.0 / 3.14159265358979323846;
        Check(std::fabs(outAng - 30.0) < 0.1, "a 30 degree push comes out at 30 degrees");
        Check(Len(p) < mag, "and comes out smaller than it went in, as the deadzone eats the bottom");
    }

    printf("\nSymmetry\n");
    {
        const StickPos a = ApplyStickDeadzone(12000, -5000);
        const StickPos b = ApplyStickDeadzone(-12000, 5000);
        Check(a.x == -b.x && a.y == -b.y, "opposite pushes come out opposite");
    }

    printf("\nOddly sized deadzones\n");
    {
        const StickPos off = ApplyStickDeadzone(500, 500, 0);
        Check(off.x == 500 && off.y == 500, "a zero deadzone passes the stick straight through");
        const StickPos neg = ApplyStickDeadzone(500, 500, -10);
        Check(neg.x == 500 && neg.y == 500, "a negative one does too");
        const StickPos huge = ApplyStickDeadzone(32767, 0, 100000);
        Check(huge.x == 32767 && huge.y == 0, "one wider than the range still reaches full travel");
    }

    printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES.");
    return g_failures == 0 ? 0 : 1;
}
