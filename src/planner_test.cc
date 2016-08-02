/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 */
#include "planner.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gtest/gtest.h>

#include "gcode-parser.h"
#include "hardware-mapping.h"
#include "logging.h"
#include "motor-operations.h"
#include "gcode-machine-control.h"

#define VERBOSE_ENQUEUE 0

// To identify problems with axis speed assignments we have each
// axis a significant different number of steps/mm
// Use 1 for no change and e.g. 8 for significant change.
#define STEP_FACTOR_BETWEEN_AXES 8

// Set up config that they are the same for all the tests.
static void InitTestConfig(struct MachineControlConfig *c) {
  float steps_per_mm = 1000;
  for (int i = 0; i <= AXIS_Z; ++i) {
    c->steps_per_mm[i] = steps_per_mm;
    // We do different steps/mm to detect problems when going between
    // euclidian space and step-space.
    steps_per_mm *= STEP_FACTOR_BETWEEN_AXES;
    c->acceleration[i] = 100;  // mm/s^2
    c->max_feedrate[i] = 10000;
  }
  c->threshold_angle = 0;
  c->require_homing = false;
}

class FakeMotorOperations : public MotorOperations {
public:
  FakeMotorOperations(const MachineControlConfig &config)
    : config_(config) {}

  virtual void Enqueue(const LinearSegmentSteps &segment_in) {
    LinearSegmentSteps segment = segment_in;

#if VERBOSE_ENQUEUE
    {
      // figure out what the defining axis is the v0 and v1 refer to.
      // Real world coordinates so that the euclid_len.
      const float dx = segment.steps[AXIS_X] / config_.steps_per_mm[AXIS_X];
      const float dy = segment.steps[AXIS_Y] / config_.steps_per_mm[AXIS_Y];
      const float dz = segment.steps[AXIS_Z] / config_.steps_per_mm[AXIS_Z];
      const float euclid_len = sqrtf(dx*dx + dy*dy + dz*dz);
      const float angle = 360.0f * atan2f(dy, dx) / (2*M_PI);

      float v0_euclid, v1_euclid;
      calcEuclidSpeed(segment, &v0_euclid, &v1_euclid);
      // Somewhat verbose, but useful :).
      fprintf(stderr, "  Receiving: (%6.1f, %6.1f, %6.1f); "
              "euclid len: %5.1f angle: %6.1f°;"
              "v_step: %8.1f -> %8.1f; "
              "v_euclid: %5.1f -> %5.1f\n",
              dx, dy, dz, euclid_len, angle,
              segment.v0, segment.v1, v0_euclid, v1_euclid);
    }
#endif
    collected_.push_back(segment);
  }

  virtual void MotorEnable(bool on)  {}
  virtual void WaitQueueEmpty() {}

  const std::vector<LinearSegmentSteps> &segments() { return collected_; }

  // Calculate what the step speed would be in euclidian speed, taking the
  // steps/mm in each axis into account.
  void calcEuclidSpeed(const LinearSegmentSteps &seg,
                       float *euclid_v0, float *euclid_v1) {
    const float dx = seg.steps[AXIS_X] / config_.steps_per_mm[AXIS_X];
    const float dy = seg.steps[AXIS_Y] / config_.steps_per_mm[AXIS_Y];
    const float dz = seg.steps[AXIS_Z] / config_.steps_per_mm[AXIS_Z];
    const float euclid_len = sqrtf(dx*dx + dy*dy + dz*dz);
    GCodeParserAxis defining = AXIS_X;
    if (abs(seg.steps[AXIS_Y]) > abs(seg.steps[defining])) defining = AXIS_Y;
    if (abs(seg.steps[AXIS_Z]) > abs(seg.steps[defining])) defining = AXIS_Z;
    const float defining_len = fabsf(seg.steps[defining]
                                     / config_.steps_per_mm[defining]);
    // Diagonal is moving faster than the defining axis component by this:
    const float speed_factor = euclid_len / defining_len;
    // (steps/sec)  / (steps/mm) => mm/sec
    if (euclid_v0)
      *euclid_v0 = speed_factor * seg.v0 / config_.steps_per_mm[defining];
    if (euclid_v1)
      *euclid_v1 = speed_factor * seg.v1 / config_.steps_per_mm[defining];
  }

private:
  const MachineControlConfig &config_;
  std::vector<LinearSegmentSteps> collected_;
};

class PlannerHarness {
public:
  PlannerHarness(float threshold_angle = 0)
    : motor_ops_(config_), finished_(false) {
    InitTestConfig(&config_);
    config_.threshold_angle = threshold_angle;
    simulated_hardware_.AddMotorMapping(AXIS_X, 1, false);
    simulated_hardware_.AddMotorMapping(AXIS_Y, 2, false);
    simulated_hardware_.AddMotorMapping(AXIS_Z, 3, false);
    planner_ = new Planner(&config_, &simulated_hardware_, &motor_ops_);
  }
  ~PlannerHarness() { delete planner_; }

  void Enqueue(const AxesRegister &target, float feed) {
    assert(!finished_);   // Can only call if segments() has not been called.
    //fprintf(stderr, "NewPos: (%.1f, %.1f)\n", target[AXIS_X], target[AXIS_Y]);
    planner_->Enqueue(target, feed);
  }

  const std::vector<LinearSegmentSteps> &segments() {
    if (!finished_) {
      planner_->BringPathToHalt();
      finished_ = true;
    }
    return motor_ops_.segments();
  }

private:
  MachineControlConfig config_;
  FakeMotorOperations motor_ops_;
  HardwareMapping simulated_hardware_;
  bool finished_;
  Planner *planner_;
};

// Conditions that we expect in all moves.
static void VerifyCommonExpectations(
      const std::vector<LinearSegmentSteps> &segments) {
  ASSERT_GT(segments.size(), 1) << "Expected more than one segment";

  // Some basic assumption: something is moving.
  EXPECT_GT(segments[0].v1, 0);

  // At the begin and end of our travel, we have zero speed.
  EXPECT_EQ(0, segments[0].v0);
  EXPECT_EQ(0, segments[segments.size()-1].v1);

  // The joining speeds between segments should match. Since we can be at the
  // border of switching the dominant axis, we need to first convert into
  // euclidian speed.
  for (size_t i = 0; i < segments.size()-1; ++i) {
    EXPECT_EQ(segments[i].v1, segments[i+1].v0)
      << "Joining speed between " << i << " and " << (i+1);
  }
}

TEST(PlannerTest, SimpleMove_NeverReachingFullSpeed) {
  PlannerHarness plantest;

  AxesRegister pos;
  pos[AXIS_X] = 100;
  pos[AXIS_Y] = 100;
  plantest.Enqueue(pos, 1000);  // never reach speed in short move

  // We expect two segments: accelerating to maximum speed we can
  // reach, then decelerating.
  EXPECT_EQ(2, plantest.segments().size());

  VerifyCommonExpectations(plantest.segments());
}

TEST(PlannerTest, SimpleMove_ReachesFullSpeed) {
  PlannerHarness plantest;

  AxesRegister pos;
  pos[AXIS_X] = 100;
  pos[AXIS_Y] = 100;
  plantest.Enqueue(pos, 10);

  // We expect three segments: accelerating, plateau and decelerating.
  EXPECT_EQ(3, plantest.segments().size());

  VerifyCommonExpectations(plantest.segments());
}

TEST(PlannerTest, SimpleMove_SpeedsDifferentForDifferentAxes) {
  const float base_steps_per_mm = 1000;
  const float test_speed_mm_per_sec = 10;
  {
    PlannerHarness plantest;

    AxesRegister pos;
    pos[AXIS_X] = 100;
    pos[AXIS_Y] = 0;
    plantest.Enqueue(pos, test_speed_mm_per_sec);

    // Step-speed reached depends on axis speed
    EXPECT_EQ(base_steps_per_mm * test_speed_mm_per_sec,
              plantest.segments()[1].v0);
  }
  {
    PlannerHarness plantest;

    AxesRegister pos;
    pos[AXIS_X] = 0;
    pos[AXIS_Y] = 100;
    plantest.Enqueue(pos, test_speed_mm_per_sec);

    EXPECT_EQ(base_steps_per_mm * test_speed_mm_per_sec
              * STEP_FACTOR_BETWEEN_AXES,
              plantest.segments()[1].v0);
  }

  // Here, the X-axis is the dominant axis and determines the speed.
  {
    PlannerHarness plantest;

    AxesRegister pos;
    pos[AXIS_X] = 100;
    pos[AXIS_Y] = 100 / STEP_FACTOR_BETWEEN_AXES - 1;  // <- will not be dominant
    const float total_len = hypotf(pos[AXIS_X], pos[AXIS_Y]);
    plantest.Enqueue(pos, test_speed_mm_per_sec);

    // Here, AXIS_X is still
    EXPECT_FLOAT_EQ((pos[AXIS_X] / total_len)
                    * base_steps_per_mm * test_speed_mm_per_sec,
                    plantest.segments()[1].v0);
  }

  // Here, we just flipped to the Y-Axis being dominant
  {
    PlannerHarness plantest;

    AxesRegister pos;
    pos[AXIS_X] = 100;
    pos[AXIS_Y] = 100 / STEP_FACTOR_BETWEEN_AXES + 1;  // <- will be dominant
    const float total_len = hypotf(pos[AXIS_X], pos[AXIS_Y]);
    plantest.Enqueue(pos, test_speed_mm_per_sec);

    EXPECT_FLOAT_EQ((pos[AXIS_Y] / total_len)
                    * base_steps_per_mm * test_speed_mm_per_sec
                    * STEP_FACTOR_BETWEEN_AXES,
                    plantest.segments()[1].v0);
  }

}

static std::vector<LinearSegmentSteps> DoAngleMove(float threshold_angle,
                                                   float start_angle,
                                                   float delta_angle,
                                                   float feedrate,
                                                   float len_factor) {
#if VERBOSE_ENQUEUE
  fprintf(stderr, "DoAngleMove(%.1f, %.1f, %.1f) @ %.1f %s\n",
          threshold_angle, start_angle, delta_angle, feedrate,
          (len_factor < 1) ? "shorter exit move" :
          (len_factor > 1) ? "longer exit move" :
          "equal length moves");
#endif
  PlannerHarness plantest(threshold_angle);
  const float kSegmentLen = 100;

  float radangle = 2 * M_PI * start_angle / 360;
  AxesRegister pos;
  pos[AXIS_X] = kSegmentLen * cos(radangle) + pos[AXIS_X];
  pos[AXIS_Y] = kSegmentLen * sin(radangle) + pos[AXIS_Y];
  plantest.Enqueue(pos, feedrate);

  radangle += 2 * M_PI * delta_angle / 360;
  pos[AXIS_X] = (kSegmentLen * len_factor) * cos(radangle) + pos[AXIS_X];
  pos[AXIS_Y] = (kSegmentLen * len_factor) * sin(radangle) + pos[AXIS_Y];
  plantest.Enqueue(pos, feedrate);
  std::vector<LinearSegmentSteps> segments = plantest.segments();
  VerifyCommonExpectations(segments);
  return segments;
}

TEST(PlannerTest, CornerMove_90Degrees) {
  const float kFeedrate = 3000.0f;  // Never reached. We go from accel to decel.
  const float kThresholdAngle = 5.0f;
  std::vector<LinearSegmentSteps> segments =
    DoAngleMove(kThresholdAngle, 0, 90, kFeedrate, 1);

  ASSERT_EQ(4, segments.size());

  // This is a 90 degree move, we expect to slow down all the way to zero
  // in the elbow.
  EXPECT_EQ(0, segments[1].v1);
  EXPECT_EQ(segments[1].v1, segments[2].v0);
}

// kFeedrate  kLenFactor  Result
// 3000.0     1.0         Passes with glitch where final move does not reach 0 speed
//                          The first move is all accel. The second move is
//                          sometimes to short to fully decel.
// 3000.0     1.1         Passes
//                          The first move is all accel. The second move is
//                          always long enough to decel
// 100.0      1.0         Passes
//                          The first move accels to full speed, moves at that
//                          speed, then decels to the max speed of the second
//                          move. The second move moves at max speed then decels.
// 100.0      0.5         Passes
//                          Same as above. The second move just has a shorter
//                          move segment.
// 100.0      0.4         Passes with glitch where final move does not reach 0 speed
//                           The second move is sometimes to short to fully decel.
//
// If USE_DIFFERENT_STEP_SPEEDS is enabled, the tests still pass. But the
// glitches start going away as the angles get closer to the axis with
// higher resolution (i.e. there are more steps to do the decel).
void testShallowAngleAllStartingPoints(float threshold, float testing_angle) {
  const float kFeedrate = 3000.0f;
  const float kLenFactor = 1.0f;
  // Essentially, we go around the circle as starting segments.
  for (float angle = 0; angle < 360; angle += threshold/2) {
    std::vector<LinearSegmentSteps> segments =
      DoAngleMove(threshold, angle, testing_angle, kFeedrate, kLenFactor);

    // No matter what we get at least 2 segments
    ASSERT_GT(segments.size(), 1);

    // VerifyCommonExpectations() already checked the "knots" between segments

    // A shallow move just plows through the middle, so we expect a larger
    // speed than zero
    EXPECT_GT(segments[0].v1, 0);
  }
}

TEST(PlannerTest, CornerMove_Shallow_PositiveAngle) {
  const float kThresholdAngle = 5.0f;
  const float kTestingAngle = 0.7 * kThresholdAngle;
  testShallowAngleAllStartingPoints(kThresholdAngle, kTestingAngle);
}

TEST(PlannerTest, CornerMove_Shallow_NegativeAngle) {
  const float kThresholdAngle = 5.0f;
  const float kTestingAngle = -0.7 * kThresholdAngle;
  testShallowAngleAllStartingPoints(kThresholdAngle, kTestingAngle);
}

int main(int argc, char *argv[]) {
  Log_init("/dev/stderr");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
