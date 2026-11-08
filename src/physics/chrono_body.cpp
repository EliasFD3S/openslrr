// Project Chrono (https://projectchrono.org) rigid-body integrator for the
// SLRR chassis. The suspension, tyre and contact stages in
// vehicle_dynamics.cpp still produce forces/impulses on the RigidBody mirror;
// Chrono owns the time integration of the 6-DOF chassis (gravity, applied
// wrench, gyroscopic coupling) with its implicit Euler stepper.
#include "slrr/vehicle_dynamics.hpp"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystemNSC.h>

#include <cmath>

namespace slrr::physics {
namespace ch = ::chrono;

struct ChronoBodyState {
  ch::ChSystemNSC system;
  std::shared_ptr<ch::ChBody> body;
  unsigned accumulator = 0;
  ChronoBodyState() {
    system.SetGravitationalAcceleration(ch::ChVector3d(0, -9.81, 0));
    system.SetTimestepperType(
        ch::ChTimestepper::Type::EULER_IMPLICIT_LINEARIZED);
    body = chrono_types::make_shared<ch::ChBody>();
    body->EnableCollision(false);
    body->SetSleepingAllowed(false);
    system.AddBody(body);
    accumulator = body->AddAccumulator();
  }
};

BackendHandle::BackendHandle() = default;
BackendHandle::~BackendHandle() = default;
// A copied RigidBody rebuilds its own Chrono system on the next step.
BackendHandle::BackendHandle(const BackendHandle &) {}
BackendHandle &BackendHandle::operator=(const BackendHandle &) {
  state_.reset();
  return *this;
}
BackendHandle::BackendHandle(BackendHandle &&) noexcept = default;
BackendHandle &BackendHandle::operator=(BackendHandle &&) noexcept = default;
void BackendHandle::reset(ChronoBodyState *state) { state_.reset(state); }
const char *backend_name() { return "chrono"; }

namespace {
ch::ChVector3d to_ch(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 from_ch(const ch::ChVector3d &v) { return {v.x(), v.y(), v.z()}; }
} // namespace

struct ChronoIntegrator {
  static void integrate(RigidBody &rb, double dt) {
    if (!rb.backend_.get())
      rb.backend_.reset(new ChronoBodyState);
    auto &state = *rb.backend_.get();
    auto &body = *state.body;

    // Push the mirror: it may have been edited by the caller or by contact
    // impulses since the previous step.
    const Vec3 inv = rb.inverse_inertia_;
    body.SetMass(rb.mass);
    body.SetInertiaXX({1 / inv.x, 1 / inv.y, 1 / inv.z});
    body.SetPos(to_ch(rb.position));
    const auto &q = rb.orientation;
    body.SetRot(ch::ChQuaterniond(q.w, q.x, q.y, q.z));
    body.SetPosDt(to_ch(rb.velocity));
    body.SetAngVelParent(to_ch(rb.omega()));

    body.EmptyAccumulator(state.accumulator);
    body.AccumulateForce(state.accumulator, to_ch(rb.force_),
                         to_ch(rb.position), false);
    body.AccumulateTorque(state.accumulator, to_ch(rb.torque_), false);

    state.system.DoStepDynamics(dt);

    // Pull the integrated state back into the mirror.
    rb.position = from_ch(body.GetPos());
    const auto &r = body.GetRot();
    rb.orientation = Quaternion{r.e1(), r.e2(), r.e3(), r.e0()}.normalized();
    rb.velocity = from_ch(body.GetPosDt());
    // World angular momentum L = R I R^T w, then the native angular drag.
    const Vec3 w =
        rb.orientation.inverse_rotate(from_ch(body.GetAngVelParent()));
    rb.momentum =
        rb.orientation.rotate({w.x / inv.x, w.y / inv.y, w.z / inv.z}) *
        std::exp(-.08 * dt);
  }
};

// Out-of-line hook used by RigidBody::integrate.
void chrono_integrate(RigidBody &rb, double dt) {
  ChronoIntegrator::integrate(rb, dt);
}
} // namespace slrr::physics
