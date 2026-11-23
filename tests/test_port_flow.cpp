// Run after installing patch/parts in a private data/game copy:
// powershell -File scripts/run_save_io_tests.ps1 -Suite port_flow
// Executes the shipped Java helper and fuel system in the VM, not C++
// duplicates of them.
#include "standalone_test.hpp"
#include "slrr/jvm.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>

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
  double heat = 0, min_compression = 0, max_compression = 0;
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
  return {number("mixture_H"), number("minComp"), number("maxComp")};
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

SLRR_TEST(fuel_heat_keeps_the_stock_values_and_compression_window) {
  PortVm f;
  const auto close = [](double got, double want) { return std::abs(got - want) <= want * 1e-5; };
  struct Fuel {
    int type;
    float optimal;
    double stock;  // the stock heat value, J/kg
  };
  // #352 restored the stock heat (x1.55) so engine mods keep their scale.
  for (const Fuel fuel : {Fuel{0, 12.5f, 1099500}, Fuel{1, 13.5f, 1100000},
                          Fuel{2, 14.f, 1100250}, Fuel{3, 16.f, 1400000},
                          Fuel{4, 11.f, 1600000}}) {
    for (float ratio : {fuel.optimal, 9.f, 17.f}) {
      const double match = mixture_match(ratio, fuel.optimal);
      const auto got = fuel_heat(f, fuel.type, ratio);
      SLRR_REQUIRE(close(got.heat, fuel.stock * match * 1.55));
      // The compression gates keep the stock x1.55 heat value.
      SLRR_REQUIRE(close(got.min_compression, fuel.stock * match * 1.55 / 250000));
      SLRR_REQUIRE(close(got.max_compression, fuel.stock * match * 1.55 / 100000));
    }
  }
  const auto pump = fuel_heat(f, 0, 12.5f);
  std::printf("[port_flow] 95 octane at 12.5:1: %.0f J/kg, compression %.4f-%.4f\n",
              pump.heat, pump.min_compression, pump.max_compression);
}

int main() {
  const slrr::standalone::Test tests[] = {
      curtain_geometry_has_linear_lift_and_a_throat_limit,
      reference_exchange_volume_agrees_in_si_and_script_units,
      missing_geometry_or_restriction_closes_only_that_port,
      fuel_heat_keeps_the_stock_values_and_compression_window,
  };
  return slrr::standalone::run("port_flow", tests);
}
