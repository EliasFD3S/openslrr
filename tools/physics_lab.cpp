// Physics Lab (#380): an offline, bit-reproducible test bench for the vehicle
// solver on a flat plane. It drives the game's VehicleDynamics with scripted
// inputs at any solver rate and prints one summary line per scenario, so the
// same scenario can be compared across rates and across physics changes.
//
//   physics_lab [--hz 120,250,500,1000] [--scenario all|launch|brake|coast|skidpad|
//                step_steer|burnout|fade|split_mu|...] [--csv DIR] [--abs X] [--asr X]
//                [--respec]
//
// The car is the 1300 kg rear-drive saloon of tests/test_vehicle_dynamics.cpp
// (Baiern-like geometry, sport 205 tyres, 235 N m engine).
#include "slrr/tire_model.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {
using namespace slrr::physics;
constexpr double kKph = 3.6;

TireParameters sport_tyre(double width) {
  TireParameters t;
  t.friction = 1.559 * (.5 + .5 * width / 275) * 1.3;
  t.maximum_load = 6500;
  t.load_smoothing = .4;
  return t;
}

// --sport: the suspension of the Baiern CoupeSport 2.5 of a career save, as
// WheelRef receives it from Spring.java / ShockAbsorber.java (11 cm of travel,
// about 7 cm of it in bump from static).
bool sport_suspension = false;
// --gt3: the Baiern CoupeSport GT III as the game assembles it (1167 kg, race
// tyres mu 2.27/2.11 with stiffness 40, 122 kN/m springs with about 2.5 cm of
// travel each way, rear drive). --mixed keeps the left/right tyre swap the
// game's assembly produced.
bool gt3 = false, gt3_mixed = false;
// --bars K,D: the same anti-roll bar (N/m, N s/m) on both axles.
double bar_force = 0, bar_damping = 0;
// --respec (#446): the re-specced Baiern CoupeSport brakes, real torques at
// full pedal, 100 bar (front 286 mm vented, 57 mm piston; rear 276 mm solid,
// 42 mm; street pads) with the rear proportioning valve,
// with their rotor temperature and pad fade; otherwise 2000 N m per wheel
// and no thermal model.
bool respec_brakes = false;
// --abs X / --asr X: the Options driving aids (Config.player_abs / _asr).
double abs_strength = 0, asr_strength = 0;

struct Car {
  VehicleDynamics dynamics;
  Car() {
    dynamics.body.configure(1300, {-.85, .15, -2.2}, {.85, 1.4, 2.2}, {0, .45, .1},
                            {550, 2300, 2200});
    const double x[] = {-.75, .75, -.75, .75}, z[] = {-1.3, -1.3, 1.35, 1.35};
    for (int i = 0; i < 4; ++i) {
      auto& w = dynamics.wheels[i];
      w.enabled = true;
      w.center = {x[i], .31, z[i]};
      w.radius = .31;
      w.anchor = .35;
      w.rest = .478;
      w.minimum = .25;
      w.maximum = .45;
      w.spring = 25000;
      w.bump_damping = w.rebound_damping = 1500;
      w.unsprung = 25;
      w.half_width = .1025;  // 205 mm tyre
      if (gt3) {
        // 295 rear tyres where the game put them: left side when --mixed.
        const bool wide = gt3_mixed ? i % 2 == 0 : i >= 2;
        w.radius = wide ? .300 : .305;
        w.half_width = wide ? .147 : .127;
        w.anchor = .39;
        w.center.y = .131 + (i < 2 ? .02 : -.02);
        w.rest = i < 2 ? .187 : .214;
        w.minimum = i < 2 ? .140 : .165;
        w.maximum = i < 2 ? .191 : .216;
        w.spring = 122625;
        w.bump_damping = w.rebound_damping = i < 2 ? 6100 : 5700;
        w.unsprung = 10;
      }
      if (sport_suspension) {
        w.anchor = .39;
        w.center.y = .131;  // hub at .30 m with the static length (about .22 m)
        w.rest = w.maximum = i < 2 ? .266 : .256;
        w.minimum = .152;
        w.spring = 70632;
        w.bump_damping = w.rebound_damping = 5200;
        w.unsprung = 10;
        w.radius = .3;
        w.half_width = .108;
      }
      const double side = i % 2 ? -1 : 1;
      w.front_instant_center = w.side_instant_center = true;
      w.front_center_offset = {side * 1.446, -.025, 0};
      w.side_center_offset = {0, .1, i < 2 ? 1.5 : -1.5};
      auto& d = dynamics.drive.wheels[i];
      d.weight = i < 2 ? 0 : 1;
      d.brake = 2000;
      d.handbrake = i < 2 ? 0 : 2000;
      if (respec_brakes) {
        d.brake = i < 2 ? 2409 : 1252;
        d.handbrake = i < 2 ? 0 : 1252;
        d.hardware = brake_hardware(0, i < 2 ? 2 : 0, i < 2 ? .286 : .276);
        d.tyre_heat = tyre_thermal_spec(1, w.radius, 2 * w.half_width);  // sport tyre
      }
      d.tyre = sport_tyre(205);
      if (gt3) {
        const bool wide = gt3_mixed ? i % 2 == 0 : i >= 2;
        d.tyre.friction = wide ? 2.272 : 2.113;
        d.tyre.stiffness = 40;
        d.tyre.sliction = .836;
        d.tyre.maximum_load = wide ? 12413 : 10437;
        d.tyre.load_smoothing = .4;
      }
    }
    if (gt3) {
      dynamics.body.configure(1167, {-.87, .15, -2.1}, {.87, 1.3, 2.1}, {0, .42, .07},
                              {1788, 1969, 313});
      Matrix f{};
      f[0] = f[5] = f[10] = f[15] = 1;
      f[13] = .05f;
      dynamics.body.set_frame(f);
    }
    for (auto& bar : dynamics.anti_roll) bar = {bar_force, bar_damping};
    auto& drive = dynamics.drive;
    drive.gears = 5;
    const double ratios[] = {0, 12.9, 7.9, 5.6, 4.4, 3.6, 0, -12};
    std::copy(std::begin(ratios), std::end(ratios), drive.ratios.begin());
    drive.idle = 800;
    drive.limit = 7000;
    drive.inertia = .38;
    drive.brake_scale = drive.handbrake_scale = 1;
    if (respec_brakes) {
      drive.handbrake_scale = 2;
      drive.rear_valve_knee = .3;
      drive.rear_valve_slope = .3;
    }
    drive.abs_strength = abs_strength;
    drive.asr_strength = asr_strength;
    drive.engaged = drive.automatic_shift = drive.auto_clutch = true;
    drive.torque = [](double rpm) { return std::clamp(150 + rpm * .02, 0., 235.); };
    if (gt3) drive.torque = [](double rpm) { return rpm < 1000 ? 200 : 375.; };
    dynamics.engine_running = true;
    dynamics.engine_omega = 800 / 9.549;
    Matrix frame{};
    frame[0] = frame[5] = frame[10] = frame[15] = 1;
    frame[13] = .05f;
    dynamics.body.set_frame(frame);
  }
};

const Terrain flat = [](double, double, double, GroundHit& hit) {
  hit = {};
  return true;
};

// A kerb: the ground steps up by kerb_height past a line kerb_z ahead of
// the start (the car drives towards -z), turned by kerb_angle (radians) about
// the vertical. kerb_height 0 is the flat plane.
double kerb_height = 0, kerb_z = -12, kerb_angle = 0;
const Terrain kerb = [](double x, double z, double, GroundHit& hit) {
  hit = {};
  if (z * std::cos(kerb_angle) + x * std::sin(kerb_angle) < kerb_z) hit.height = kerb_height;
  return true;
};

// One solver step with the given inputs.
struct Inputs {
  double throttle = 0, brake = 0, steer = 0, front_brake_only = 0;
};

struct Sample {
  double t, speed, rpm, yaw, lateral_g, longitudinal_g, vertical_g;
  int gear;
  double slip[4], lat[4], fx[4], fy[4], load[4], spin[4];
  double pressure[4], temperature[4];
  double reference;
  bool abs, asr;
};

class Run {
 public:
  explicit Run(const char* name) : name_(name) {}
  Car car;
  double t = 0;
  std::vector<Sample> samples;

  double speed() const { return car.dynamics.body.velocity.length(); }

  void step(const Inputs& in) {
    auto& d = car.dynamics;
    auto& drive = d.drive;
    drive.throttle = in.throttle;
    drive.brake = in.brake;
    for (int i = 0; i < 2; ++i) drive.wheels[i].steer = in.steer;
    if (in.front_brake_only > 0)
      for (int i = 2; i < 4; ++i) drive.wheels[i].brake = 0;
    const Vec3 v0 = d.body.velocity;
    d.advance(VehicleDynamics::timestep, kerb_height > 0 ? kerb : flat);
    t += VehicleDynamics::timestep;
    const Vec3 accel = (d.body.velocity - v0) / VehicleDynamics::timestep;
    const Vec3 right = d.body.orientation.rotate({1, 0, 0});
    const Vec3 fwd = d.body.orientation.rotate({0, 0, -1});
    Sample s{};
    s.t = t;
    s.speed = speed();
    s.rpm = d.engine_omega * 9.54929658551372;
    s.gear = d.gear;
    s.yaw = d.body.omega().y;
    s.lateral_g = accel.dot(right) / 9.81;
    s.longitudinal_g = accel.dot(fwd) / 9.81;
    s.vertical_g = accel.y / 9.81;
    for (int i = 0; i < 4; ++i) {
      const auto& w = d.state[i];
      s.slip[i] = w.slip_speed;
      s.lat[i] = w.lateral_speed;
      s.fx[i] = w.tire_force.longitudinal;
      s.fy[i] = w.tire_force.lateral;
      s.load[i] = w.force;
      s.spin[i] = w.spin * d.wheels[i].radius;
      s.pressure[i] = w.brake_pressure;
      s.temperature[i] = w.brake_temperature;
    }
    s.reference = d.reference_speed();
    s.abs = d.abs_active();
    s.asr = d.asr_active();
    samples.push_back(s);
  }

  void write_csv(const std::string& dir, unsigned hz) const {
    if (dir.empty()) return;
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/" + name_ + "_" + std::to_string(hz) + ".csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "t,speed_kph,rpm,gear,yaw_rate,lat_g,long_g,reference_kph,abs,asr");
    for (int i = 0; i < 4; ++i)
      std::fprintf(f, ",slip%d,lat%d,fx%d,fy%d,load%d,wheel_speed%d,pressure%d,rotor%d", i, i, i, i,
                   i, i, i, i);
    std::fprintf(f, "\n");
    // 1 kHz and below: write at most 200 rows per second.
    const size_t stride = std::max<size_t>(1, VehicleDynamics::frequency / 200);
    for (size_t k = 0; k < samples.size(); k += stride) {
      const auto& s = samples[k];
      std::fprintf(f, "%.4f,%.3f,%.0f,%d,%.4f,%.4f,%.4f,%.3f,%d,%d", s.t, s.speed * kKph, s.rpm,
                   s.gear, s.yaw, s.lateral_g, s.longitudinal_g, s.reference * kKph, s.abs, s.asr);
      for (int i = 0; i < 4; ++i)
        std::fprintf(f, ",%.4f,%.4f,%.1f,%.1f,%.1f,%.3f,%.3f,%.1f", s.slip[i], s.lat[i], s.fx[i],
                     s.fy[i], s.load[i], s.spin[i], s.pressure[i], s.temperature[i]);
      std::fprintf(f, "\n");
    }
    std::fclose(f);
  }

 private:
  const char* name_;
};

// Holds a target speed with throttle and brake (PI on speed error).
struct SpeedHold {
  double target, integral = 0;
  Inputs operator()(double speed, double steer) {
    const double error = target - speed;
    integral = std::clamp(integral + error * VehicleDynamics::timestep, -5.0, 5.0);
    const double u = error * .5 + integral * .2;
    Inputs in;
    in.steer = steer;
    in.throttle = std::clamp(u, 0.0, 1.0);
    in.brake = std::clamp(-u, 0.0, 1.0);
    return in;
  }
};

// Accelerates from rest to the target speed at full throttle.
void reach(Run& run, double kph) {
  const double end = run.t + 40;
  while (run.speed() * kKph < kph && run.t < end) run.step({1, 0, 0});
}

void hold(Run& run, double kph, double seconds) {
  SpeedHold control{kph / kKph};
  const double end = run.t + seconds;
  while (run.t < end) run.step(control(run.speed(), 0));
}

std::string launch(const std::string& csv) {
  Run run("launch");
  double t60 = -1, t100 = -1, worst_slip = 0;
  while (run.t < 12) {
    run.step({1, 0, 0});
    const double kph = run.speed() * kKph;
    if (t60 < 0 && kph >= 60) t60 = run.t;
    if (t100 < 0 && kph >= 100) t100 = run.t;
    for (int i = 2; i < 4; ++i)
      worst_slip = std::max(worst_slip, run.samples.back().slip[i]);
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  char out[200];
  std::snprintf(out, sizeof out, "0-60 %.3f s  0-100 %.3f s  v(12s) %.2f km/h  max rear slip %.2f m/s",
                t60, t100, run.speed() * kKph, worst_slip);
  return out;
}

std::string brake(const std::string& csv) {
  Run run("brake");
  reach(run, 100);
  const Vec3 start = run.car.dynamics.body.position;
  const double t0 = run.t;
  double locked = 0, front_temperature = 0;
  while (run.speed() > .1 && run.t < t0 + 15) {
    run.step({0, 1, 0});
    for (const auto& s : run.car.dynamics.state)
      if (run.speed() > 2 && std::abs(s.spin) * .31 < .3 * run.speed()) locked += VehicleDynamics::timestep / 4;
  }
  front_temperature = run.car.dynamics.state[0].brake_temperature;
  const double rear_temperature = run.car.dynamics.state[2].brake_temperature;
  run.write_csv(csv, VehicleDynamics::frequency);
  const Vec3 d = run.car.dynamics.body.position - start;
  char out[200];
  std::snprintf(out, sizeof out, "100-0 %.2f m in %.3f s, %.2f wheel-s locked, rotors %.0f / %.0f C",
                std::hypot(d.x, d.z), run.t - t0, locked, front_temperature, rear_temperature);
  return out;
}

// Brake fade: ten stops from 150 to 30 km/h at full pedal, accelerating back
// between them. Reports the first and last stop's distance and deceleration
// and the hottest front rotor.
std::string fade(const std::string& csv) {
  Run run("fade");
  double first = 0, last = 0, hottest = 0, first_g = 0, last_g = 0;
  for (int stop = 0; stop < 10; ++stop) {
    reach(run, 150);
    const Vec3 start = run.car.dynamics.body.position;
    const double t0 = run.t;
    while (run.speed() * kKph > 30 && run.t < t0 + 15) run.step({0, 1, 0});
    const Vec3 d = run.car.dynamics.body.position - start;
    const double distance = std::hypot(d.x, d.z);
    const double g = (150 - 30) / kKph / std::max(1e-3, run.t - t0) / 9.81;
    if (stop == 0) first = distance, first_g = g;
    last = distance, last_g = g;
    hottest = std::max(hottest, run.car.dynamics.state[0].brake_temperature);
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  char out[200];
  std::snprintf(out, sizeof out, "stop 1 %.1f m %.2f g, stop 10 %.1f m %.2f g, hottest front %.0f C",
                first, first_g, last, last_g, hottest);
  return out;
}

// Ten minutes at 100 km/h: the tyres' steady temperatures.
std::string cruise(const std::string& csv) {
  Run run("cruise");
  reach(run, 100);
  hold(run, 100, 600);
  run.write_csv(csv, VehicleDynamics::frequency);
  const auto& st = run.car.dynamics.state;
  char out[200];
  std::snprintf(out, sizeof out, "front tread %.0f C carcass %.0f C, rear tread %.0f C carcass %.0f C",
                st[0].tyre_temperature.surface, st[0].tyre_temperature.carcass,
                st[2].tyre_temperature.surface, st[2].tyre_temperature.carcass);
  return out;
}

// Split friction: the left wheels on ice (friction x 0.25) during a full
// stop from 100 km/h, the driver steering to hold the heading (at most
// 0.15 rad, reacting after 0.3 s). Reports the stop, the largest yaw rate
// and the heading change.
std::string split_mu(const std::string& csv) {
  Run run("split_mu");
  reach(run, 100);
  for (int i : {0, 2}) {
    run.car.dynamics.drive.wheels[i].tyre.friction *= .25;
  }
  const Vec3 start = run.car.dynamics.body.position;
  const double t0 = run.t;
  double yaw = 0;
  const auto heading = [&] {
    const Vec3 f = run.car.dynamics.body.orientation.rotate({0, 0, -1});
    return std::atan2(f.x, -f.z);
  };
  const double heading0 = heading();
  while (run.speed() > .1 && run.t < t0 + 15) {
    Inputs in{0, 1, 0};
    if (run.t - t0 > .3)
      in.steer = std::clamp(2 * (heading() - heading0) - .3 * run.samples.back().yaw, -.15, .15);
    run.step(in);
    yaw = std::max(yaw, std::abs(run.samples.back().yaw));
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  const Vec3 d = run.car.dynamics.body.position - start;
  char out[200];
  std::snprintf(out, sizeof out, "100-0 %.2f m, yaw max %.2f rad/s, heading change %.1f deg",
                std::hypot(d.x, d.z), yaw, (heading() - heading0) * 57.29578);
  return out;
}

// Lifts off at 100 km/h and coasts: the engine braking and the gears the
// automatic box picks for it (#448).
std::string coast(const std::string& csv) {
  Run run("coast");
  reach(run, 100);
  const size_t first = run.samples.size();
  const double t0 = run.t;
  while (run.speed() * kKph > 10 && run.t < t0 + 90) run.step({0, 0, 0});
  run.write_csv(csv, VehicleDynamics::frequency);
  std::string out;
  for (double kph : {90.0, 60.0, 40.0, 20.0}) {
    double g = 0, rpm = 0;
    int n = 0, gear = 0;
    for (size_t k = first; k < run.samples.size(); ++k) {
      const auto& s = run.samples[k];
      if (std::abs(s.speed * kKph - kph) > 2) continue;
      g += s.longitudinal_g, rpm += s.rpm, gear = s.gear, ++n;
    }
    char part[80];
    std::snprintf(part, sizeof part, "%s%.0f: %.3f g G%d %.0f rpm", out.empty() ? "" : " | ", kph,
                  n ? -g / n : 0., gear, n ? rpm / n : 0.);
    out += part;
  }
  char tail[60];
  std::snprintf(tail, sizeof tail, " | to 10 km/h %.1f s", run.t - t0);
  return out + tail;
}

std::string skidpad(const std::string& csv) {
  Run run("skidpad");
  reach(run, 60);
  hold(run, 60, 2);
  SpeedHold control{60 / kKph};
  double best = 0, best_steer = 0;
  const double t0 = run.t;
  while (run.t < t0 + 20) {
    const double steer = .2 * (run.t - t0) / 20;  // ramp to 0.2 rad
    run.step(control(run.speed(), steer));
    const double g = std::abs(run.samples.back().lateral_g);
    if (g > best) { best = g; best_steer = steer; }
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  const auto& st = run.car.dynamics.state;
  char out[240];
  std::snprintf(out, sizeof out,
                "max lateral %.3f g at steer %.3f rad, final speed %.1f km/h, treads %.0f/%.0f/%.0f/%.0f C",
                best, best_steer, run.speed() * kKph, st[0].tyre_temperature.surface,
                st[1].tyre_temperature.surface, st[2].tyre_temperature.surface,
                st[3].tyre_temperature.surface);
  return out;
}

std::string step_steer(const std::string& csv) {
  Run run("step_steer");
  reach(run, 80);
  hold(run, 80, 2);
  SpeedHold control{80 / kKph};
  const double t0 = run.t;
  double peak = 0, t_peak = 0, settled = 0;
  int settled_n = 0;
  while (run.t < t0 + 4) {
    run.step(control(run.speed(), .04));
    const double r = std::abs(run.samples.back().yaw);
    if (run.t - t0 <= 2 && r > peak) { peak = r; t_peak = run.t - t0; }
    if (run.t - t0 > 3) { settled += r; ++settled_n; }  // steady state: last second
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  const double final = settled_n ? settled / settled_n : 0;
  char out[200];
  std::snprintf(out, sizeof out, "yaw peak %.4f rad/s at %.3f s, steady %.4f, overshoot %.1f %%",
                peak, t_peak, final, final > 0 ? (peak / final - 1) * 100 : 0);
  return out;
}

// Drives at a steady speed into a 15 cm kerb, square or at an angle, and
// reports whether both axles climbed it, the bump (peak upward body
// acceleration), the time with no wheel on the ground and the peak
// deceleration the kerb caused.
std::string kerb_case(const char* name, const std::string& csv, double kph, double degrees) {
  Run run(name);
  SpeedHold control{kph / kKph};
  while (run.t < 8) run.step(control(run.speed(), 0));
  auto& d = run.car.dynamics;
  if (sport_suspension) {
    // The 8 lowest body points the game gives a car (left/right, four along):
    // a lowered coupe's lips 12 cm and sills 14 cm above the road.
    const double zs[] = {-2.15, -.8, .8, 2.15}, clearance[] = {.12, .14, .14, .12};
    d.body_support_count = 0;
    for (double x : {-.6, .6})
      for (int k = 0; k < 4; ++k)
        d.body_supports[d.body_support_count++] = {x, clearance[k] - d.body.position.y + d.body.center.y, zs[k]};
  }
  // The kerb line 6 m ahead of the front wheels.
  kerb_height = .15;
  kerb_angle = degrees * 3.141592653589793 / 180;
  kerb_z = (d.body.position.z - 1.3 - 6) * std::cos(kerb_angle) +
           d.body.position.x * std::sin(kerb_angle);
  const double t0 = run.t;
  double climbed_at = -1, min_speed = 1e9, bump = 0, decel = 0, airborne = 0; int flips = 0;
  bool last[4];
  for (int i = 0; i < 4; ++i) last[i] = d.state[i].contact;
  std::vector<double> window;
  double window_sum = 0, bump10 = 0;
  while (run.t < t0 + 2 + 30 / kph) {
    run.step(control(run.speed(), 0));
    const auto& sample = run.samples.back();
    min_speed = std::min(min_speed, run.speed() * kKph);
    bump = std::max(bump, sample.vertical_g);
    window.push_back(sample.vertical_g);
    window_sum += sample.vertical_g;
    if (window.size() > std::max<size_t>(1, VehicleDynamics::frequency / 100)) {
      window_sum -= window.front();
      window.erase(window.begin());
    }
    bump10 = std::max(bump10, window_sum / window.size());
    decel = std::max(decel, -sample.longitudinal_g);
    bool any = false;
    for (const auto& w : d.state) any = any || w.contact;
    if (!any) airborne += VehicleDynamics::timestep;
    // Contact changes: a wheel leaving or touching the ground.
    for (int i = 0; i < 4; ++i) {
      if (d.state[i].contact != last[i]) ++flips;
      last[i] = d.state[i].contact;
    }
    // Both axles on the kerb: the rear wheels (z +1.35 from the body) past it.
    const double along = (d.body.position.z + 1.35) * std::cos(kerb_angle) +
                         d.body.position.x * std::sin(kerb_angle);
    if (climbed_at < 0 && along < kerb_z - 1) climbed_at = run.t - t0;
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  kerb_height = kerb_angle = 0;
  char out[200];
  std::snprintf(out, sizeof out,
                "%s at %.2f s, bump %.2f g (10 ms %.2f g), airborne %.0f ms, peak decel %.2f g, min %.1f km/h, contact changes %d",
                climbed_at >= 0 ? "climbed" : "STUCK", climbed_at, bump, bump10, airborne * 1e3, decel,
                min_speed, flips);
  return out;
}
std::string curb(const std::string& csv) { return kerb_case("curb", csv, 8, 0); }
std::string curb_fast(const std::string& csv) { return kerb_case("curb_fast", csv, 30, 0); }
std::string curb_angle(const std::string& csv) { return kerb_case("curb_angle", csv, 20, 20); }
std::string curb_side(const std::string& csv) { return kerb_case("curb_side", csv, 20, 8); }

// The steady-state tyre curves of the Lab tyre at 3500 N and 20 m/s: peak
// lateral friction and its slip angle, what is left at 20 degrees, peak
// longitudinal friction and its slip ratio, a locked wheel, and the lateral
// friction left at 6 degrees with 20 % wheelspin (combined slip).
std::string tyre_curve(const std::string& csv) {
  auto tyre = sport_tyre(205);
  if (gt3) {  // the GT III rear tyre
    tyre.friction = 2.272;
    tyre.stiffness = 40;
    tyre.sliction = .836;
    tyre.maximum_load = 12413;
  }
  constexpr double load = 3500, road = 20, pi = 3.141592653589793;
  const auto lateral_mu = [&](double degrees, double kappa, double camber = 0) {
    const double lateral = road * std::tan(degrees * pi / 180);
    return std::abs(tire_forces(load, 1, road * (1 + kappa), road, lateral, tyre,
                                camber * pi / 180).lateral) / load;
  };
  const auto longitudinal_mu = [&](double kappa) {
    return std::abs(tire_forces(load, 1, road * (1 + kappa), road, 0, tyre).longitudinal) / load;
  };
  double peak_y = 0, peak_alpha = 0, peak_x = 0, peak_kappa = 0;
  FILE* f = nullptr;
  if (!csv.empty()) {
    std::filesystem::create_directories(csv);
    f = std::fopen((csv + "/tyre_curve.csv").c_str(), "w");
    if (f) std::fprintf(f, "slip_angle_deg,mu_y,slip_ratio,mu_x%c", 10);
  }
  for (int i = 0; i <= 600; ++i) {
    const double alpha = i * .05, kappa = -i * .002;  // braking side
    const double y = lateral_mu(alpha, 0), x = longitudinal_mu(kappa);
    if (y > peak_y) { peak_y = y; peak_alpha = alpha; }
    if (x > peak_x) { peak_x = x; peak_kappa = -kappa; }
    if (f && i % 5 == 0) std::fprintf(f, "%.2f,%.4f,%.3f,%.4f%c", alpha, y, -kappa, x, 10);
  }
  if (f) std::fclose(f);
  // Camber: the lateral friction at 4 degrees with the top leaning 3 degrees
  // into the force (negative camber on the outside tyre), and away from it.
  const double into = lateral_mu(4, 0, -3), away = lateral_mu(4, 0, 3);
  char out[320];
  std::snprintf(out, sizeof out,
                "lat peak %.3f at %.1f deg, at 10 deg %.3f, at 20 deg %.3f | long peak %.3f at %.1f %%, "
                "locked %.3f | lat at 6 deg: %.3f, with 20 %% spin %.3f | 4 deg camber -3: %.3f, 0: %.3f, +3: %.3f",
                peak_y, peak_alpha, lateral_mu(10, 0), lateral_mu(20, 0), peak_x, peak_kappa * 100,
                longitudinal_mu(-1), lateral_mu(6, 0), lateral_mu(6, .2), into, lateral_mu(4, 0), away);
  return out;
}

// Power oversteer: a steady 40 km/h turn, then full throttle for 2 s with
// the same steering. Reports the largest rear slip angle and yaw rate, and
// whether the car spun (rear slip angle past 45 degrees).
std::string power_oversteer(const std::string& csv) {
  Run run("power_oversteer");
  SpeedHold control{40 / kKph};
  const double steer = .12;
  while (run.t < 10) run.step(control(run.speed(), steer));
  const double t0 = run.t, yaw0 = std::abs(run.samples.back().yaw);
  double rear = 0, yaw = 0;
  while (run.t < t0 + 2) {
    Inputs in;
    in.throttle = 1;
    in.steer = steer;
    run.step(in);
    const auto& d = run.car.dynamics;
    const Vec3 v = d.body.point_velocity(d.state[2].patch);
    const Vec3 fwd = d.body.orientation.rotate({0, 0, -1});
    const Vec3 right = d.body.orientation.rotate({1, 0, 0});
    rear = std::max(rear, std::abs(std::atan2(v.dot(right), std::abs(v.dot(fwd)))) * 57.29578);
    yaw = std::max(yaw, std::abs(run.samples.back().yaw));
  }
  run.write_csv(csv, VehicleDynamics::frequency);
  char out[200];
  std::snprintf(out, sizeof out, "rear slip angle max %.1f deg, yaw %.2f -> %.2f rad/s%s", rear,
                yaw0, yaw, rear > 45 ? " (spun)" : "");
  return out;
}

std::string burnout(const std::string& csv) {
  Run run("burnout");
  double spin = 0, moved = 0;
  const Vec3 start = run.car.dynamics.body.position;
  while (run.t < 4) {
    Inputs in{1, 1, 0, 1};
    run.step(in);
    const auto& s = run.samples.back();
    spin = std::max(spin, (s.spin[2] + s.spin[3]) * .5);
  }
  const Vec3 d = run.car.dynamics.body.position - start;
  moved = std::hypot(d.x, d.z);
  run.write_csv(csv, VehicleDynamics::frequency);
  char out[200];
  std::snprintf(out, sizeof out, "rear wheel speed max %.2f m/s, car moved %.3f m, rear tread %.0f C",
                spin, moved, run.car.dynamics.state[2].tyre_temperature.surface);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<unsigned> rates = {120, 250, 500, 1000};
  std::string only = "all", csv;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--hz") && i + 1 < argc) {
      rates.clear();
      std::string list = argv[++i];
      for (size_t p = 0; p < list.size();) {
        const size_t q = list.find(',', p);
        rates.push_back(static_cast<unsigned>(std::stoul(list.substr(p, q - p))));
        p = q == std::string::npos ? list.size() : q + 1;
      }
    } else if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) {
      only = argv[++i];
    } else if (!std::strcmp(argv[i], "--asr") && i + 1 < argc) {
      asr_strength = std::atof(argv[++i]);
    } else if (!std::strcmp(argv[i], "--abs") && i + 1 < argc) {
      abs_strength = std::atof(argv[++i]);
    } else if (!std::strcmp(argv[i], "--respec")) {
      respec_brakes = true;
    } else if (!std::strcmp(argv[i], "--gt3")) {
      gt3 = true;
    } else if (!std::strcmp(argv[i], "--bars") && i + 1 < argc) {
      std::sscanf(argv[++i], "%lf,%lf", &bar_force, &bar_damping);
    } else if (!std::strcmp(argv[i], "--mixed")) {
      gt3_mixed = true;
    } else if (!std::strcmp(argv[i], "--sport")) {
      sport_suspension = true;
    } else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) {
      csv = argv[++i];
    } else {
      std::fprintf(stderr, "usage: physics_lab [--hz 120,1000] [--scenario NAME] [--csv DIR] [--asr X] [--abs X] [--respec] [--sport]\n");
      return 2;
    }
  }
  struct Scenario { const char* name; std::function<std::string(const std::string&)> run; };
  const Scenario scenarios[] = {{"launch", launch}, {"brake", brake}, {"coast", coast}, {"skidpad", skidpad},
                                {"step_steer", step_steer}, {"burnout", burnout},
                                {"curb", curb}, {"curb_fast", curb_fast},
                                {"curb_angle", curb_angle}, {"curb_side", curb_side},
                                {"tyre_curve", tyre_curve},
                                {"power_oversteer", power_oversteer}, {"fade", fade},
                                {"split_mu", split_mu}, {"cruise", cruise}};
  for (const auto& s : scenarios) {
    if (only != "all" && only != s.name) continue;
    for (unsigned hz : rates) {
      VehicleDynamics::set_frequency(hz);
      std::printf("%-10s %5u Hz  %s\n", s.name, hz, s.run(csv).c_str());
    }
  }
  return 0;
}
