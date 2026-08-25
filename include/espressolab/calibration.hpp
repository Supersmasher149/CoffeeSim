#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "espressolab/result.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/types.hpp"

// Section 11.3: calibration is an explicit, separate workflow. Nothing here runs
// as part of a normal simulation, and none of it looks at taste descriptions.
namespace espressolab::calibration {

// One point from a real shot. Pressure is optional because most setups can
// measure time and mass but not pressure (11.3).
struct MeasuredSample {
    double time_s = 0.0;
    double beverage_mass_g = 0.0;
    std::optional<double> pressure_bar;
};

struct MeasuredShot {
    std::string id;
    // Set by the loader to the file stem. A shot can be named on the command
    // line by either its id or its filename, because both are things a person
    // reasonably remembers.
    std::string source_stem;
    Recipe recipe;
    std::vector<MeasuredSample> series;
    std::optional<double> final_beverage_mass_g;
    std::optional<double> final_shot_time_s;
    std::optional<double> final_tds_percent;
    std::string machine;
    std::string date;
    std::string notes;

    // True when the file was produced by `espressolab_cli synthesize`. Synthetic
    // shots exercise the machinery; they can never justify a claim about the
    // real world, so the flag follows them into every report.
    bool synthetic = false;

    [[nodiscard]] ValidationResult validate() const;
};

// Section 11.4. Weights are in the units of their terms, so the defaults below
// put a gram of mass error, a second of time error and a tenth of a TDS point
// on roughly equal footing.
struct LossWeights {
    double mass = 1.0;         // per gram RMSE of the beverage-mass curve
    double time = 0.5;         // per second of stop-time error
    double tds = 10.0;         // per percentage point of final TDS error
    double regularization = 100.0;  // per unit outside a parameter's bounds
};

struct LossBreakdown {
    double mass_rmse_g = 0.0;
    double time_error_s = 0.0;
    double tds_error_percent = 0.0;
    double regularization = 0.0;
    double total = 0.0;
    bool simulated = false;  // false when the candidate failed validation
};

// A coefficient the fit is allowed to move, with the bounds that keep it
// physical. Wide-ranging quantities are searched in log space.
struct TunableParameter {
    std::string name;
    double low = 0.0;
    double high = 1.0;
    bool logarithmic = false;
};

// The parameters this build knows how to fit, with their bounds.
std::vector<std::string> tunable_parameter_names();
std::optional<TunableParameter> tunable_parameter(const std::string& name);
double read_parameter(const ModelCoefficients& coefficients, const std::string& name);
ModelCoefficients with_parameter(const ModelCoefficients& coefficients, const std::string& name,
                                 double value);

struct CalibrationSpec {
    ModelCoefficients starting_point;
    std::vector<MeasuredShot> fitting_shots;
    // Held back and never used for tuning: a fit that only reproduces its own
    // training shots has told you nothing (11.3).
    std::vector<MeasuredShot> validation_shots;
    std::vector<TunableParameter> parameters;
    LossWeights weights;
    SimulationConfig config;
    int maximum_iterations = 400;
    double tolerance = 1.0e-6;
};

struct ShotLoss {
    std::string shot_id;
    LossBreakdown loss;
};

struct CalibrationReport {
    ModelCoefficients fitted;
    double starting_loss = 0.0;
    double final_loss = 0.0;
    int iterations = 0;
    int simulations = 0;
    bool converged = false;

    std::vector<ShotLoss> fitting_losses;
    std::vector<ShotLoss> validation_losses;   // computed, never optimised
    double validation_loss = 0.0;

    std::vector<TunableParameter> parameters;
    std::vector<double> starting_values;
    std::vector<double> fitted_values;

    // True when any shot involved was synthetic.
    bool used_synthetic_data = false;
};

// Mean loss of one coefficient set against one shot.
LossBreakdown evaluate_shot_loss(const MeasuredShot& shot, const ModelCoefficients& coefficients,
                                 const SimulationConfig& config, const LossWeights& weights);

// Deterministic Nelder-Mead over the normalised parameter box. The same spec
// always produces the same fit.
CalibrationReport fit(const CalibrationSpec& spec);

// Linear interpolation of a simulated mass curve at an arbitrary time, so a
// measured sample at 7.3 s can be compared against a 0.05 s sample grid.
double interpolate_beverage_mass_g(const std::vector<ShotSample>& samples, double time_s);

namespace io {

MeasuredShot load_measured_shot_file(const std::filesystem::path& file,
                                     const std::filesystem::path& recipe_base);
std::vector<MeasuredShot> load_measured_shot_directory(const std::filesystem::path& directory);
std::string dump_report_json(const CalibrationReport& report, int indent = 2);

// Writes a fitted coefficient file with provenance: which shots, which
// parameters, which held-out validation case, and whether the data was
// synthetic (11.3, "Commit the coefficient file with its dataset reference").
std::string dump_fitted_coefficients_json(const CalibrationReport& report,
                                          const std::string& id, const std::string& version,
                                          const std::vector<std::string>& fitting_shot_ids,
                                          const std::vector<std::string>& validation_shot_ids,
                                          int indent = 2);

// Dev utility behind `espressolab_cli synthesize`: runs the model and writes the
// output in the measured-shot format, flagged synthetic.
std::string dump_synthetic_shot_json(const Recipe& recipe, const ShotResult& result,
                                     const std::string& recipe_path, double noise_g,
                                     unsigned int seed, int indent = 2);

}  // namespace io

}  // namespace espressolab::calibration
