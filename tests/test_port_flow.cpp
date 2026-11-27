// Run after installing patch/parts in a private data/game copy:
// powershell -File scripts/run_save_io_tests.ps1 -Suite port_flow
// Executes the shipped Java helper and fuel system in the VM, not C++
// duplicates of them.
#include "standalone_test.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/jvm.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <unordered_map>

namespace {
using namespace slrr;
constexpr const char* kHead =
    "java.game.parts.enginepart.slidingenginepart.reciprocatingenginepart.CylinderHead";

struct PortVm {
  NativeRegistry natives;
  Jvm vm{&natives};
  Object* head = nullptr;
  PortVm() {
    const auto root = std::filesystem::absolute("data/game");
    SLRR_REQUIRE(std::filesystem::is_directory(root));
    vm.set_game_root(root.string());
    SLRR_REQUIRE(vm.bootstrap_classpath());
    SLRR_REQUIRE(vm.load_class(kHead));
    head = vm.new_object(kHead);
    SLRR_REQUIRE(head);
    vm.set_halt_on_unbound(true);
  }
  double coefficient(std::array<float, 6> p) {
    std::vector<Value> args{Value::make_obj(head)};
    for (float x : p) args.push_back(Value::make_float(x));
    vm.clear_unbound();
    const auto value = vm.invoke(kHead, "portFlowCoefficient", "(FFFFFF)F", args, false);
    SLRR_REQUIRE(vm.unbound_native().empty());
    SLRR_REQUIRE(value.tag == ValueTag::Float && std::isfinite(value.v.f));
    return value.v.f;
  }
  double geometry(double diameter_mm, double lift_mm, double valves = 2,
                  double volume_cc = 1000, double efficiency = .75,
                  double wear_factor = 1, double discharge = .9) {
    // Match the stock head's cm conversion and rounded pi.
    const double disk = wear_factor * valves * 3.1416 * diameter_mm * diameter_mm * .0025;
    return coefficient({float(disk * lift_mm * .1), float(diameter_mm), float(disk),
                        float(volume_cc), float(efficiency), float(discharge)});
  }
};

bool near(double a, double b) {
  return std::abs(a - b) <= 2e-6 * std::max({.001, std::abs(a), std::abs(b)});
}

// The shipped AirFuelDeliverySystem.updatevariables, run in the VM.
struct FuelHeat {
  double heat = 0, min_compression = 0, max_compression = 0, knock_compression = 0,
         optimal = 0;
};
FuelHeat fuel_heat(PortVm& f, int type, float ratio) {
  constexpr const char* kFuel = "java.game.parts.enginepart.AirFuelDeliverySystem";
  SLRR_REQUIRE(f.vm.load_class(kFuel));
  Object* fuel = f.vm.new_object(kFuel);
  SLRR_REQUIRE(fuel);
  fuel->fields["fuel_type"] = Value::make_int(type);
  fuel->fields["mixture_ratio"] = Value::make_float(ratio);
  f.vm.clear_unbound();
  f.vm.invoke(kFuel, "updatevariables", "()V", {Value::make_obj(fuel)}, false);
  SLRR_REQUIRE(f.vm.unbound_native().empty());
  const auto number = [&](const char* name) {
    const auto& value = fuel->fields[name];
    SLRR_REQUIRE(value.tag == ValueTag::Float && std::isfinite(value.v.f));
    return static_cast<double>(value.v.f);
  };
  return {number("mixture_H"), number("minComp"), number("maxComp"), number("knockComp"),
          number("optimal_mixture_ratio")};
}
// The stock mixture-match factor 0.8 + 0.2 * match.
double mixture_match(double ratio, double optimal) {
  double detuning = ratio < optimal ? ratio / optimal : optimal / ratio;
  detuning = .45 * detuning + .55;
  detuning *= detuning;
  detuning = detuning * detuning * detuning;
  return .8 + .2 * detuning;
}
} // namespace

SLRR_TEST(curtain_geometry_has_linear_lift_and_a_throat_limit) {
  PortVm f;
  const double one = f.geometry(32, 1);
  SLRR_REQUIRE(one > 0 && one < 1);
  SLRR_REQUIRE(f.geometry(32, 0) == 0);
  SLRR_REQUIRE(near(f.geometry(32, 2), 2 * one));
  SLRR_REQUIRE(near(f.geometry(32, 1, 4), 2 * one));
  SLRR_REQUIRE(near(f.geometry(32, 1, 2, 2000), one / 2));
  SLRR_REQUIRE(near(f.geometry(32, 1, 2, 1000, .375), one / 2));
  SLRR_REQUIRE(near(f.geometry(32, 1, 2, 1000, .75, .9), .9 * one));
  // Geometric similitude doubles all lengths, so area/volume halves.
  SLRR_REQUIRE(near(f.geometry(64, 2, 2, 8000), one / 2));
  SLRR_REQUIRE(near(f.geometry(32, 8), f.geometry(32, 16)));
  SLRR_REQUIRE(f.geometry(32, 100, 2, 1) == 1);
}

SLRR_TEST(reference_exchange_volume_agrees_in_si_and_script_units) {
  PortVm f;
  constexpr double gamma = 1.4, r = 287.05, temperature = 293.15;
  const double speed = std::sqrt(gamma * r * temperature) *
      std::pow(2 / (gamma + 1), (gamma + 1) / (2 * (gamma - 1)));
  for (double lift : {.25, 1., 4., 8., 14.}) {
    constexpr double diameter = .032, n = 2, volume = .001, eta = .75, cd = .9;
    const double area = std::min(n * std::numbers::pi * diameter * lift * .001,
                                n * std::numbers::pi * diameter * diameter / 4);
    const double si = eta * cd * area * speed * .001 / volume;
    // Account for the scripts' 3.1416 instead of pi.
    SLRR_REQUIRE(std::abs(f.geometry(32, lift) - si) < si * 1e-5);
  }
  const auto kraftwerk = f.geometry(31, 11.9, 2, 452.3904, .75);
  const auto devil = f.geometry(33, 11.9, 2, 597.777365, .9375);
  const auto duty = f.geometry(42.5, 19, 2, 674.34444, .92);
  const auto duty_min = f.geometry(42.5, 1.27, 2, 674.34444, .92);
  SLRR_REQUIRE(near(kraftwerk, .447382176));
  SLRR_REQUIRE(near(devil, .479586662));
  SLRR_REQUIRE(near(duty, .691976448));
  SLRR_REQUIRE(near(duty_min, .082711538));
  std::printf("[port_flow] stock geometry: Kraftwerk %.9f Devil %.9f "
              "SuperDuty %.9f closed %.9f\n", kraftwerk, devil, duty, duty_min);
}

SLRR_TEST(missing_geometry_or_restriction_closes_only_that_port) {
  PortVm f;
  const std::array<float, 6> valid{3, 32, 16, 500, .75f, .9f};
  SLRR_REQUIRE(f.coefficient(valid) > 0);
  for (size_t i = 0; i < valid.size(); ++i) {
    auto missing = valid;
    missing[i] = 0;
    SLRR_REQUIRE(f.coefficient(missing) == 0);
    missing[i] = -1;
    SLRR_REQUIRE(f.coefficient(missing) == 0);
  }
  // Closed/minimum lift is also a flow area: worse efficiency must not
  // increase leakage by being used as a denominator.
  auto restricted = valid;
  restricted[4] *= .5f;
  SLRR_REQUIRE(near(f.coefficient(restricted), f.coefficient(valid) * .5));
}

SLRR_TEST(fuel_heat_follows_the_ethanol_blend_and_octane_sets_the_knock_limit) {
  PortVm f;
  const auto close = [](double got, double want) { return std::abs(got - want) <= want * 1e-5; };
  // Ethanol by volume to mass: 0.789 and 0.745 kg/l.
  const auto blend = [](double volume, double gasoline, double ethanol) {
    const double w = volume * .789 / (volume * .789 + (1 - volume) * .745);
    return (1 - w) * gasoline + w * ethanol;
  };
  struct Fuel {
    int type;
    double lhv, stoich, lambda, knock, min, max;
  };
  const Fuel fuels[] = {
      {0, blend(.05, 43.4e6, 26.8e6), blend(.05, 14.7, 9), .86, 10.5, 6.5, 16},
      {1, blend(.10, 43.4e6, 26.8e6), blend(.10, 14.7, 9), .86, 11.5, 6.5, 17},
      {2, 43.4e6, 14.7, .86, 12.5, 6.5, 18},
      {3, 19.9e6, 6.47, .78, 15, 8, 20.5},
      {4, 43.4e6, 14.5, 1.25, 0, 14, 24}};
  SLRR_REQUIRE(std::abs(fuels[0].stoich - 14.40) < .01 && std::abs(fuels[1].stoich - 14.10) < .01);
  for (const Fuel& fuel : fuels) {
    const double optimal = fuel.stoich * fuel.lambda;
    for (float ratio : {float(optimal), 4.5f, 9.f, 17.f}) {
      const auto got = fuel_heat(f, fuel.type, ratio);
      SLRR_REQUIRE(close(got.optimal, optimal));
      SLRR_REQUIRE(close(got.heat, fuel.lhv / fuel.stoich * mixture_match(ratio, optimal)));
      // The compression window no longer moves with the mixture.
      SLRR_REQUIRE(close(got.min_compression, fuel.min));
      SLRR_REQUIRE(close(got.max_compression, fuel.max));
      SLRR_REQUIRE(fuel.knock == 0 ? got.knock_compression == 0
                                   : close(got.knock_compression, fuel.knock));
    }
  }
  // Methanol's best power (about 5:1) is inside the mixture range; 4:1 is its floor.
  const auto methanol = fuel_heat(f, 3, 5.0466f);
  SLRR_REQUIRE(fuel_heat(f, 3, 4.f).heat < methanol.heat);
  SLRR_REQUIRE(close(fuel_heat(f, 3, 3.f).heat, fuel_heat(f, 3, 4.f).heat));
  for (int type = 0; type < 4; ++type) {
    const auto best = fuel_heat(f, type, float(fuels[type].stoich * fuels[type].lambda));
    std::printf("[port_flow] fuel %d: best %.2f:1, %.0f J/kg, knock %.1f, window %.1f-%.1f\n",
                type, best.optimal, best.heat, best.knock_compression, best.min_compression,
                best.max_compression);
  }
}

SLRR_TEST(knock_retards_above_the_fuel_limit_with_boost) {
  // Synthetic DynoData defaults: static compression 10.506:1.
  using Fields = std::unordered_map<std::string, float>;
  const auto peak = [](Fields fields) {
    int key = 0;
    slrr::DynoStats stats;
    fields["maxRPM"] = 7000.f;
    SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 28.f, &stats));
    slrr::dyno_forget(&key);
    SLRR_REQUIRE(std::abs(stats.compression - 10.5058) < 1e-3);
    return double(stats.max_torque);
  };
  const double clean = peak({});
  // No limit, or a limit above the engine, changes nothing.
  SLRR_REQUIRE(peak({{"knockCompression", 0.f}}) == clean);
  SLRR_REQUIRE(peak({{"knockCompression", 10.6f}}) == clean);
  const double knocking = peak({{"knockCompression", 8.5f}});
  const double severe = peak({{"knockCompression", 4.f}});
  SLRR_REQUIRE(knocking < clean && knocking > .9 * clean);
  SLRR_REQUIRE(severe < knocking && severe > .5 * clean);
  // Boost raises the charge pressure at TDC: the same fuel knocks.
  const Fields turbo{{"rpm_turbo_mul", 1.f}, {"rpm_turbo_opt", 3500.f},
                     {"rpm_turbo_range", 3000.f}, {"P_turbo_max", .8f}};
  auto boosted = turbo;
  boosted["knockCompression"] = 10.6f;
  const double boost_clean = peak(turbo);
  const double boost_knock = peak(boosted);
  SLRR_REQUIRE(boost_knock < boost_clean);
  // Intercooling lowers the charge temperature and the knock.
  auto cooled = boosted;
  cooled["intercooling"] = .9f;
  auto cooled_clean = turbo;
  cooled_clean["intercooling"] = .9f;
  SLRR_REQUIRE(peak(cooled) / peak(cooled_clean) > boost_knock / boost_clean);
  std::printf("[port_flow] knock: N/A %.2f Nm, limit 8.5 %.2f, limit 4 %.2f; "
              "0.8 bar %.2f, limit 10.6 %.2f\n",
              clean, knocking, severe, boost_clean, boost_knock);
}

SLRR_TEST(positive_displacement_blower_boosts_from_low_rpm_and_costs_drive_power) {
  // Synthetic DynoData defaults with an E750-like blower: drive ratio 1.5,
  // 0.5 bar at full speed, the stock turbo-law window kept for comparison.
  using Fields = std::unordered_map<std::string, float>;
  struct Row { double torque[8]; float boost[8]; };
  const float rpms[8] = {1000, 1500, 2000, 3000, 4000, 5000, 6000, 6750};
  const auto run = [&](Fields fields) {
    int key = 0;
    slrr::DynoStats stats;
    fields["maxRPM"] = 7000.f;
    SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 28.f, &stats));
    Row row{};
    for (int i = 0; i < 8; ++i) {
      row.torque[i] = slrr::dyno_sample(&key, rpms[i], 0, false);
      row.boost[i] = slrr::dyno_boost_fraction(&key, rpms[i]);
    }
    slrr::dyno_forget(&key);
    return row;
  };
  const Fields blower{{"rpm_turbo_mul", 1.5f}, {"rpm_turbo_opt", 4950.f},
                      {"rpm_turbo_range", 4050.f}, {"P_turbo_max", .5f}, {"P_turbo_waste", .5f}};
  auto roots = blower;
  roots.insert({{"charger_type", 1.f}, {"charger_leak_rpm", 300.f}, {"charger_efficiency", .55f},
                {"charger_pulley_radius", .03f}, {"charger_belt_force", 1620.f}});
  const Row na = run({}), legacy = run(blower), pd = run(roots);
  for (int i = 1; i < 8; ++i) {
    // Delivery follows blower speed: boost rises with rpm, never in a bell.
    SLRR_REQUIRE(pd.boost[i] >= pd.boost[i - 1]);
    SLRR_REQUIRE(pd.torque[i] > na.torque[i]);
  }
  // Already most of its boost at 2,000 rpm, where the turbo law has little.
  SLRR_REQUIRE(pd.boost[2] > .6f && pd.boost[2] > legacy.boost[2]);
  SLRR_REQUIRE(pd.boost[0] > legacy.boost[0]);
  // A less efficient blower heats the charge more and takes more drive power.
  auto hot = roots;
  hot["charger_efficiency"] = .35f;
  const Row worse = run(hot);
  for (int i = 2; i < 8; ++i) SLRR_REQUIRE(worse.torque[i] < pd.torque[i]);
  // A belt that cannot carry the drive slips: less boost, less torque at speed.
  auto weak = roots;
  weak["charger_belt_force"] = 60.f;
  const Row slipping = run(weak);
  SLRR_REQUIRE(slipping.boost[6] < pd.boost[6] && slipping.torque[6] < pd.torque[6]);
  // Without a belt limit (0) the blower is never held back.
  auto free_belt = roots;
  free_belt["charger_belt_force"] = 0.f;
  const Row unlimited = run(free_belt);
  for (int i = 0; i < 8; ++i) SLRR_REQUIRE(unlimited.torque[i] >= pd.torque[i] - 1e-3);
  std::printf("[port_flow] blower Nm at 1000/2000/4000/6000: NA %.0f/%.0f/%.0f/%.0f, "
              "turbo law %.0f/%.0f/%.0f/%.0f, roots %.0f/%.0f/%.0f/%.0f, slipping belt %.0f at 6000\n",
              na.torque[0], na.torque[2], na.torque[4], na.torque[6], legacy.torque[0],
              legacy.torque[2], legacy.torque[4], legacy.torque[6], pd.torque[0], pd.torque[2],
              pd.torque[4], pd.torque[6], slipping.torque[6]);
}

int main() {
  const slrr::standalone::Test tests[] = {
      curtain_geometry_has_linear_lift_and_a_throat_limit,
      reference_exchange_volume_agrees_in_si_and_script_units,
      missing_geometry_or_restriction_closes_only_that_port,
      fuel_heat_follows_the_ethanol_blend_and_octane_sets_the_knock_limit,
      knock_retards_above_the_fuel_limit_with_boost,
      positive_displacement_blower_boosts_from_low_rpm_and_costs_drive_power,
  };
  return slrr::standalone::run("port_flow", tests);
}
