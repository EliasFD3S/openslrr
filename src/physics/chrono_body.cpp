// Project Chrono (https://projectchrono.org) rigid-body integration and
// chassis/static-world contacts for SLRR. Suspension and tyre stages still
// produce forces on the RigidBody mirror; Chrono owns integration and, when a
// CollisionMesh is supplied, contact constraints for the chassis and rigid
// wheels. No second native contact pass is applied in that mode.
#include "slrr/vehicle_dynamics.hpp"

#include <chrono/collision/ChCollisionShapeBox.h>
#include <chrono/collision/ChCollisionShapeSphere.h>
#include <chrono/collision/ChCollisionShapeTriangleMesh.h>
#include <chrono/geometry/ChTriangleMeshSoup.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystemNSC.h>

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace slrr::physics {
namespace ch = ::chrono;
namespace {
ch::ChVector3d to_ch(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 from_ch(const ch::ChVector3d &v) { return {v.x(), v.y(), v.z()}; }

struct Collider {
  bool sphere = false;
  Vec3 offset, size;
  double radius = 0;
  bool operator==(const Collider &) const = default;
};

std::shared_ptr<ch::ChContactMaterial> material(double restitution) {
  auto result =
      ch::ChContactMaterial::DefaultMaterial(ch::ChContactMethod::NSC);
  result->SetFriction(.6f);
  result->SetRestitution(static_cast<float>(restitution));
  return result;
}
} // namespace

struct ChronoBodyState {
  ch::ChSystemNSC system;
  std::shared_ptr<ch::ChBody> body, ground;
  unsigned accumulator = 0;
  std::vector<Collider> colliders;
  std::shared_ptr<const CollisionMesh> mesh_source;

  ChronoBodyState() : ChronoBodyState({}, nullptr) {}
  ChronoBodyState(std::vector<Collider> shapes,
                  std::shared_ptr<const CollisionMesh> mesh)
      : colliders(std::move(shapes)), mesh_source(std::move(mesh)) {
    // The collision system must be selected before any bodies are added.
    system.SetCollisionSystemType(ch::ChCollisionSystem::Type::BULLET);
    system.SetGravitationalAcceleration(ch::ChVector3d(0, -9.81, 0));
    system.SetTimestepperType(
        ch::ChTimestepper::Type::EULER_IMPLICIT_LINEARIZED);
    system.GetSolver()->AsIterative()->SetMaxIterations(50);
    system.SetMinBounceSpeed(.8);

    body = chrono_types::make_shared<ch::ChBody>();
    body->SetSleepingAllowed(false);
    auto body_material = material(.12);
    auto wheel_material = material(0);
    for (const auto &collider : colliders) {
      const ch::ChFrame<> frame(to_ch(collider.offset));
      if (collider.sphere) {
        body->AddCollisionShape(
            chrono_types::make_shared<ch::ChCollisionShapeSphere>(
                wheel_material, collider.radius),
            frame);
      } else {
        body->AddCollisionShape(
            chrono_types::make_shared<ch::ChCollisionShapeBox>(
                body_material, to_ch(collider.size)),
            frame);
      }
    }

    ground = chrono_types::make_shared<ch::ChBody>();
    ground->SetFixed(true);
    unsigned triangles = 0;
    if (mesh_source) {
      auto soup = chrono_types::make_shared<ch::ChTriangleMeshSoup>();
      const auto &positions = mesh_source->positions;
      const auto &indices = mesh_source->indices;
      for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        ch::ChVector3d vertex[3];
        bool valid = true;
        for (unsigned corner = 0; corner < 3; ++corner) {
          const size_t offset =
              static_cast<size_t>(indices[i + corner]) * 3;
          if (offset + 2 >= positions.size()) {
            valid = false;
            break;
          }
          vertex[corner] = {positions[offset], positions[offset + 1],
                            positions[offset + 2]};
          valid = valid && std::isfinite(vertex[corner].x()) &&
                  std::isfinite(vertex[corner].y()) &&
                  std::isfinite(vertex[corner].z());
        }
        if (!valid ||
            (vertex[1] - vertex[0]).Cross(vertex[2] - vertex[0]).Length() <
                1e-12)
          continue;
        soup->AddTriangle(vertex[0], vertex[1], vertex[2]);
      }
      triangles = soup->GetNumTriangles();
      if (triangles) {
        ground->AddCollisionShape(
            chrono_types::make_shared<ch::ChCollisionShapeTriangleMesh>(
                material(.12), soup, true, false, .001));
      }
    }
    // A supplied empty mesh intentionally means no world contacts. A null
    // mesh instead selects the native contact fallback in VehicleDynamics.
    body->EnableCollision(!colliders.empty() && triangles > 0);
    ground->EnableCollision(triangles > 0);
    system.AddBody(body);
    system.AddBody(ground);
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

struct ChronoIntegrator {
  static std::vector<Collider> colliders_for(const VehicleDynamics &vehicle) {
    const auto &body = vehicle.body;
    std::vector<Collider> result;
    result.push_back(
        {false, (body.minimum + body.maximum) * .5 - body.center,
         body.maximum - body.minimum, 0});
    for (const auto &wheel : vehicle.wheels) {
      if (!wheel.enabled || (wheel.linked() && !wheel.rigid_contact) ||
          !(wheel.radius > 0))
        continue;
      const auto down = wheel.down.normalized();
      const auto local =
          wheel.center + down * (wheel.minimum - wheel.anchor_length());
      result.push_back(
          {true, local - body.center, {}, wheel.radius});
    }
    return result;
  }

  static void integrate(RigidBody &rb, double dt) {
    if (!rb.backend_.get())
      rb.backend_.reset(new ChronoBodyState);
    auto &state = *rb.backend_.get();
    auto &body = *state.body;

    // Push the mirror: it may have been edited by the caller since the last
    // step. The previous step's contact impulses are already part of it.
    const Vec3 inv = rb.inverse_inertia_;
    if (!(inv.x > 0) || !(inv.y > 0) || !(inv.z > 0))
      throw std::invalid_argument("rigid body is not configured");
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

    // External state and accumulator edits must invalidate Chrono's cached
    // update, otherwise the implicit stepper can use the previous wrench.
    state.system.ForceUpdate();
    state.system.DoStepDynamics(dt);

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

  static void integrate(VehicleDynamics &vehicle, double dt) {
    auto colliders = colliders_for(vehicle);
    auto *state = vehicle.body.backend_.get();
    if (!state || state->colliders != colliders ||
        state->mesh_source.get() != vehicle.collision.get()) {
      vehicle.body.backend_.reset(
          new ChronoBodyState(std::move(colliders), vehicle.collision));
    }
    integrate(vehicle.body, dt);
    vehicle.body.force_ = vehicle.body.torque_ = {};
    if (!vehicle.body.position.finite() || !vehicle.body.velocity.finite() ||
        !vehicle.body.momentum.finite())
      throw std::runtime_error("non-finite rigid state");
  }
};

// Out-of-line hooks used by RigidBody::integrate and VehicleDynamics::step.
void chrono_integrate(RigidBody &rb, double dt) {
  ChronoIntegrator::integrate(rb, dt);
}
void chrono_integrate_vehicle(VehicleDynamics &vehicle, double dt) {
  ChronoIntegrator::integrate(vehicle, dt);
}
} // namespace slrr::physics
