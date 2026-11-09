// Project Chrono (https://projectchrono.org) rigid-body integration,
// chassis/static-world contacts, and supported multi-body suspension for SLRR.
// Native suspension geometry remains the fallback for massless wheels and
// complex setHub struts. Chrono owns integration and contacts in mesh mode;
// the old native chassis contact pass is never applied on top of them.
#include "slrr/suspension_geometry.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <chrono/collision/ChCollisionShapeBox.h>
#include <chrono/collision/ChCollisionShapeSphere.h>
#include <chrono/collision/ChCollisionShapeTriangleMesh.h>
#include <chrono/core/ChRotation.h>
#include <chrono/geometry/ChTriangleMeshSoup.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChLinkMate.h>
#include <chrono/physics/ChLinkRevolute.h>
#include <chrono/physics/ChLinkTSDA.h>
#include <chrono/physics/ChSystemNSC.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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

std::shared_ptr<ch::ChContactMaterial> material(double restitution,
                                                double friction = .6) {
  auto result =
      ch::ChContactMaterial::DefaultMaterial(ch::ChContactMethod::NSC);
  result->SetFriction(static_cast<float>(friction));
  result->SetRestitution(static_cast<float>(restitution));
  return result;
}

ch::ChQuaterniond to_ch(Quaternion q) { return {q.w, q.x, q.y, q.z}; }
Quaternion from_ch(const ch::ChQuaterniond &q) {
  return {q.e1(), q.e2(), q.e3(), q.e0()};
}

// Chrono owns endpoint motion; this callback only evaluates the native SLRR
// spring law at the actual hub coordinate. Positive TSDA force separates the
// endpoints. The local motion ratio converts TSDA extension rate to s_dot.
class SuspensionForce final : public ch::ChLinkTSDA::ForceFunctor {
public:
  SuspensionForce(WheelConfig wheel, Vec3 center,
                  std::shared_ptr<ch::ChBody> chassis,
                  std::shared_ptr<ch::ChBody> wheel_body)
      : wheel_(std::move(wheel)), center_(center),
        chassis_(std::move(chassis)), wheel_body_(std::move(wheel_body)) {}

  double evaluate(double, double, double, double velocity,
                  const ch::ChLinkTSDA &link) override {
    const Vec3 hub = current_hub();
    const Vec3 direction =
        from_ch(link.GetPoint2Abs() - link.GetPoint1Abs()).normalized();
    const Vec3 local_direction =
        from_ch(chassis_->GetRot()).inverse_rotate(direction);
    const double effective = suspension_hub_length(wheel_, hub);
    const double ratio = suspension_motion_ratio(wheel_, hub, local_direction);
    if (!std::isfinite(ratio) || std::fabs(ratio) < 1e-6)
      return 0;
    const double speed = velocity * ratio;
    double force = suspension_force(
        effective, speed, wheel_.rest, wheel_.maximum, wheel_.spring,
        wheel_.bump_damping, wheel_.rebound_damping);
    if (effective < wheel_.minimum)
      force += (wheel_.minimum - effective) * wheel_.spring * 8;
    if (effective > wheel_.maximum)
      force -= (effective - wheel_.maximum) * wheel_.spring * 8;
    // Virtual work: F_TSDA * dL = F_SLRR * ds.
    return force * ratio;
  }

private:
  Vec3 current_hub() const {
    return center_ + from_ch(chassis_->GetRot()).inverse_rotate(
                         from_ch(wheel_body_->GetPos() - chassis_->GetPos()));
  }

  WheelConfig wheel_;
  Vec3 center_;
  std::shared_ptr<ch::ChBody> chassis_, wheel_body_;
};

struct WheelRuntime {
  std::shared_ptr<ch::ChBody> body;
  std::shared_ptr<ch::ChLinkTSDA> spring;
};

class WheelContacts final : public ch::ChContactContainer::ReportContactCallback {
public:
  explicit WheelContacts(const ch::ChContactable *target) : target_(target) {}

  bool OnReportContact(const ch::ChVector3d &pA, const ch::ChVector3d &pB,
                       const ch::ChMatrix33<> &plane, double, double,
                       const ch::ChVector3d &reaction,
                       const ch::ChVector3d &, ch::ChContactable *a,
                       ch::ChContactable *b, int) override {
    const bool is_a = a == target_, is_b = b == target_;
    if (!is_a && !is_b)
      return true;
    // Chrono applies plane*reaction to B and its negative to A.
    const Vec3 force = from_ch(plane * reaction) * (is_a ? -1.0 : 1.0);
    const double magnitude = force.length();
    if (!(magnitude > 1e-9) || !std::isfinite(magnitude))
      return true;
    // Wheel materials have zero tangential friction, so each magnitude is a
    // normal reaction. Sum them without allowing resultant cancellation.
    load += magnitude;
    if (magnitude > strongest) {
      strongest = magnitude;
      normal = force / magnitude;
      patch = from_ch(is_a ? pA : pB);
    }
    return true;
  }

  double load = 0, strongest = 0;
  Vec3 normal{0, 1, 0}, patch;

private:
  const ch::ChContactable *target_;
};
} // namespace

struct ChronoBodyState {
  ch::ChSystemNSC system;
  std::shared_ptr<ch::ChBody> body, ground;
  unsigned accumulator = 0;
  std::vector<Collider> colliders;
  std::shared_ptr<const CollisionMesh> mesh_source;
  std::array<WheelConfig, 4> suspension_config{};
  uint64_t suspension_generation = 0;
  bool has_suspension_config = false;
  std::array<WheelRuntime, 4> wheels;

  ChronoBodyState() { initialize(nullptr, {}, nullptr); }
  ChronoBodyState(const VehicleDynamics &vehicle,
                  std::vector<Collider> shapes,
                  std::shared_ptr<const CollisionMesh> mesh) {
    initialize(&vehicle, std::move(shapes), std::move(mesh));
  }

  void initialize(const VehicleDynamics *vehicle,
                  std::vector<Collider> shapes,
                  std::shared_ptr<const CollisionMesh> mesh) {
    colliders = std::move(shapes);
    mesh_source = std::move(mesh);
    // The collision system must be selected before any bodies are added.
    system.SetCollisionSystemType(ch::ChCollisionSystem::Type::BULLET);
    system.SetGravitationalAcceleration(ch::ChVector3d(0, -9.81, 0));
    system.SetTimestepperType(
        ch::ChTimestepper::Type::EULER_IMPLICIT_LINEARIZED);
    system.GetSolver()->AsIterative()->SetMaxIterations(50);
    system.SetMinBounceSpeed(.8);

    body = chrono_types::make_shared<ch::ChBody>();
    body->SetSleepingAllowed(false);
    if (vehicle) {
      const auto &rb = vehicle->body;
      body->SetPos(to_ch(rb.position));
      body->SetRot(to_ch(rb.orientation));
      body->SetPosDt(to_ch(rb.velocity));
      body->SetAngVelParent(to_ch(rb.omega()));
    }
    auto body_material = material(.12);
    auto rigid_wheel_material = material(0);
    for (const auto &collider : colliders) {
      const ch::ChFrame<> frame(to_ch(collider.offset));
      if (collider.sphere) {
        body->AddCollisionShape(
            chrono_types::make_shared<ch::ChCollisionShapeSphere>(
                rigid_wheel_material, collider.radius),
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
    // Family 1 contains independent suspension bodies. They collide with the
    // ground, not with their own chassis or each other. A bare rigid body
    // (a detached panel) has no shapes and therefore no collision model.
    if (auto model = body->GetCollisionModel())
      model->DisallowCollisionsWith(1);
    system.AddBody(body);
    system.AddBody(ground);
    accumulator = body->AddAccumulator();

    if (!vehicle)
      return;
    suspension_config = vehicle->wheels;
    suspension_generation = vehicle->suspension_generation;
    has_suspension_config = true;
    if (!mesh_source)
      return;
    const auto &rb = vehicle->body;
    for (size_t i = 0; i < vehicle->wheels.size(); ++i) {
      const auto &wheel = vehicle->wheels[i];
      if (!wheel.enabled || wheel.rigid_contact || !(wheel.unsprung > 0) ||
          wheel.has_hub || !(wheel.spring > 0) || !(wheel.radius > 0) ||
          wheel.maximum < wheel.minimum)
        continue;
      const auto down = wheel.down.normalized();
      if (down.length() < .9)
        continue;
      const double reference_seed =
          std::clamp(wheel.rest, wheel.minimum, wheel.maximum);
      // Construction must make exactly the same stable ownership decision as
      // VehicleDynamics. External seeds only choose the initial pose.
      if (!chrono_suspension_geometry(wheel, reference_seed))
        continue;
      const auto &previous = vehicle->state[i];
      double initial =
          std::clamp(previous.seeded ? previous.length : wheel.rest,
                     wheel.minimum, wheel.maximum);
      // Invalid external seeds are reinitialized, never used to transfer a
      // live corner to native ownership. Rest is also the stable eligibility
      // reference used by VehicleDynamics.
      if (!chrono_suspension_geometry(wheel, initial))
        initial = reference_seed;
      const auto ray =
          wheel.center + down * (initial - wheel.anchor_length());
      Vec3 physical_local;
      if (!hinge_hub(wheel, ray, physical_local))
        continue;
      const Vec3 physical_world =
          rb.position + rb.orientation.rotate(physical_local - rb.center);
      // Keep endpoints distinct even at zero suspension compression.
      const double top_mount = std::max(.1, wheel.maximum);
      const Vec3 spring_anchor =
          wheel.center - down * (wheel.anchor_length() + top_mount);
      const Vec3 hard_local = spring_anchor - rb.center;
      const Vec3 hard_world =
          rb.position + rb.orientation.rotate(hard_local);
      const double reference =
          (physical_world - hard_world).length() + wheel.rest - initial;
      if (!(reference > 1e-6) || !std::isfinite(reference))
        continue;

      if (!chrono_suspension_geometry(wheel, initial))
        continue;

      const bool revolute = wheel.arm_length > 0;
      const Vec3 axis = revolute ? wheel.arm_axis.normalized() : down;
      if (axis.length() < .9)
        continue;

      auto &runtime = wheels[i];
      runtime.body = chrono_types::make_shared<ch::ChBody>();
      runtime.body->SetMass(wheel.unsprung);
      const double inertia = .4 * wheel.unsprung * wheel.radius * wheel.radius;
      runtime.body->SetInertiaXX({inertia, inertia, inertia});
      runtime.body->SetPos(to_ch(physical_world));
      runtime.body->SetRot(to_ch(rb.orientation));
      runtime.body->SetPosDt(to_ch(rb.point_velocity(physical_world)));
      runtime.body->SetAngVelParent(to_ch(rb.omega()));
      runtime.body->SetSleepingAllowed(false);
      // SLRR tyre forces remain external. Chrono supplies only the normal
      // contact load for these wheels, avoiding duplicate tangential response.
      runtime.body->AddCollisionShape(
          chrono_types::make_shared<ch::ChCollisionShapeSphere>(
              material(0, 0), wheel.radius));
      runtime.body->GetCollisionModel()->SetFamily(1);
      runtime.body->GetCollisionModel()->DisallowCollisionsWith(1);
      runtime.body->EnableCollision(triangles > 0);
      system.AddBody(runtime.body);

      const Vec3 joint_position =
          revolute
              ? rb.position + rb.orientation.rotate(wheel.arm_pivot - rb.center)
              : physical_world;
      const ch::ChFrame<> joint_frame(
          to_ch(joint_position),
          ch::QuatFromVec2Vec(
              ch::ChVector3d(0, 0, 1),
              to_ch(rb.orientation.rotate(axis))));
      if (revolute) {
        auto joint = chrono_types::make_shared<ch::ChLinkRevolute>();
        joint->Initialize(body, runtime.body, joint_frame);
        system.AddLink(joint);
      } else {
        auto joint = chrono_types::make_shared<ch::ChLinkMatePrismatic>();
        joint->Initialize(body, runtime.body, joint_frame);
        system.AddLink(joint);
      }

      runtime.spring = chrono_types::make_shared<ch::ChLinkTSDA>();
      runtime.spring->Initialize(body, runtime.body, true, to_ch(hard_local),
                                 ch::ChVector3d(0, 0, 0));
      runtime.spring->SetRestLength(reference);
      runtime.spring->RegisterForceFunctor(
          chrono_types::make_shared<SuspensionForce>(
              wheel, rb.center, body, runtime.body));
      system.AddLink(runtime.spring);
    }
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
  static constexpr double kMaxSubstepRate = 720;

  static std::vector<Collider> colliders_for(const VehicleDynamics &vehicle) {
    const auto &body = vehicle.body;
    std::vector<Collider> result;
    result.push_back(
        {false, (body.minimum + body.maximum) * .5 - body.center,
         body.maximum - body.minimum, 0});
    for (const auto &wheel : vehicle.wheels) {
      // Only rigid wheels are fused to the chassis. Native sprung wheels
      // already have spring/stop forces; fusing them here would add a second
      // support path. Constrained wheels have independent collision bodies.
      if (!wheel.enabled || !wheel.rigid_contact || !(wheel.radius > 0))
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

    // Push only the chassis mirror. Suspension bodies retain their own dynamic
    // state; overwriting them from render mirrors would discard the solution.
    const Vec3 inv = rb.inverse_inertia_;
    if (!(inv.x > 0) || !(inv.y > 0) || !(inv.z > 0))
      throw std::invalid_argument("rigid body is not configured");
    body.SetMass(rb.mass);
    body.SetInertiaXX({1 / inv.x, 1 / inv.y, 1 / inv.z});
    body.SetPos(to_ch(rb.position));
    body.SetRot(to_ch(rb.orientation));
    body.SetPosDt(to_ch(rb.velocity));
    body.SetAngVelParent(to_ch(rb.omega()));

    body.EmptyAccumulator(state.accumulator);
    body.AccumulateForce(state.accumulator, to_ch(rb.force_),
                         to_ch(rb.position), false);
    body.AccumulateTorque(state.accumulator, to_ch(rb.torque_), false);

    // Driving runs at 120 Hz. Suspension springs and wheel contacts need a
    // finer Chrono step, so substep with the step's external wrench held.
    const int substeps =
        std::max(1, static_cast<int>(std::ceil(dt * kMaxSubstepRate - 1e-9)));
    for (int i = 0; i < substeps; ++i) {
      state.system.ForceUpdate();
      state.system.DoStepDynamics(dt / substeps);
    }

    rb.position = from_ch(body.GetPos());
    const auto &r = body.GetRot();
    rb.orientation = Quaternion{r.e1(), r.e2(), r.e3(), r.e0()}.normalized();
    rb.velocity = from_ch(body.GetPosDt());
    const Vec3 w =
        rb.orientation.inverse_rotate(from_ch(body.GetAngVelParent()));
    rb.momentum =
        rb.orientation.rotate({w.x / inv.x, w.y / inv.y, w.z / inv.z}) *
        std::exp(-.08 * dt);
  }

  static void sync_wheels(VehicleDynamics &vehicle, ChronoBodyState &state) {
    for (size_t i = 0; i < state.wheels.size(); ++i) {
      auto &runtime = state.wheels[i];
      auto &mirror = vehicle.state[i];
      if (!runtime.body) {
        mirror.constrained = false;
        continue;
      }
      const auto &wheel = vehicle.wheels[i];
      const Vec3 center = from_ch(runtime.body->GetPos());
      auto contacts = chrono_types::make_shared<WheelContacts>(runtime.body.get());
      state.system.GetContactContainer()->ReportAllContacts(contacts);
      const double load = contacts->load;
      mirror.constrained = true;
      mirror.seeded = true;
      mirror.contact = load > 1e-3;
      mirror.normal_load = load;
      if (mirror.contact)
        mirror.normal = contacts->normal;
      mirror.center = center;
      mirror.hub = vehicle.body.center + vehicle.body.orientation.inverse_rotate(
                                               center - vehicle.body.position);
      mirror.patch = mirror.contact
                         ? contacts->patch
                         : center - mirror.normal * wheel.radius;
      mirror.length = suspension_hub_length(wheel, mirror.hub);
      const Vec3 direction =
          from_ch(runtime.spring->GetPoint2Abs() -
                  runtime.spring->GetPoint1Abs())
              .normalized();
      const double ratio =
          suspension_motion_ratio(wheel, mirror.hub,
                       vehicle.body.orientation.inverse_rotate(direction));
      mirror.speed = runtime.spring->GetVelocity() * ratio;
      // Preserve native support semantics and undo the TSDA virtual-work
      // scale. Unsprung weight remains only in normal_load.
      mirror.force = mirror.contact && std::isfinite(ratio) &&
                             std::fabs(ratio) > 1e-6
                         ? std::max(0.0, runtime.spring->GetForce() / ratio)
                         : 0.0;
      if (!center.finite() || !std::isfinite(mirror.length) ||
          !std::isfinite(mirror.speed) || !std::isfinite(load))
        throw std::runtime_error("non-finite Chrono suspension state");
    }
  }

  static void integrate(VehicleDynamics &vehicle, double dt,
                        bool mesh_contacts) {
    auto colliders = colliders_for(vehicle);
    auto *state = vehicle.body.backend_.get();
    const auto &mesh = mesh_contacts ? vehicle.collision : nullptr;
    if (!state || state->colliders != colliders ||
        state->mesh_source.get() != mesh.get() ||
        !state->has_suspension_config ||
        state->suspension_generation != vehicle.suspension_generation ||
        state->suspension_config != vehicle.wheels) {
      vehicle.body.backend_.reset(
          new ChronoBodyState(vehicle, std::move(colliders), mesh));
    }
    integrate(vehicle.body, dt);
    sync_wheels(vehicle, *vehicle.body.backend_.get());
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
void chrono_integrate_vehicle(VehicleDynamics &vehicle, double dt,
                              bool mesh_contacts) {
  ChronoIntegrator::integrate(vehicle, dt, mesh_contacts);
}
} // namespace slrr::physics
