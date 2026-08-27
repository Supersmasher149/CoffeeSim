#include "espressolab/cfd3d_artifact_io.hpp"

#include <bit>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab::cfd3d_artifact_io {
namespace {

using nlohmann::json;

constexpr std::size_t kFieldCount = 7;
constexpr std::size_t kMaximumSnapshots = 128;
constexpr std::uint64_t kMaximumFieldBytes = 1ULL << 30;
constexpr std::size_t kChunkHeaderBytes = 64;

[[noreturn]] void fail(const char* code, const std::string& path, const std::string& message) {
    throw artifact_io::LoadError(code, path, message);
}

json parse_json(const std::string& text, const std::string& path) {
    try {
        return json::parse(text);
    } catch (const json::parse_error& error) {
        fail("MALFORMED_JSON", path, error.what());
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) fail("FILE_NOT_FOUND", path.string(), "could not open " + path.string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) fail("WRITE_FAILED", path.string(), "could not write " + path.string());
    stream << contents;
    if (!stream) fail("WRITE_FAILED", path.string(), "could not write " + path.string());
}

void require_object(const json& value, const std::string& path) {
    if (!value.is_object()) fail("MALFORMED_JSON", path, path + " must be an object");
}

int integer_value(const json& object, const char* key, const std::string& path, int fallback) {
    if (!object.contains(key)) return fallback;
    if (!object.at(key).is_number_integer()) {
        fail("MALFORMED_JSON", path + "." + key, std::string(key) + " must be an integer");
    }
    return object.at(key).get<int>();
}

double number_value(const json& object, const char* key, const std::string& path,
                   double fallback) {
    if (!object.contains(key)) return fallback;
    if (!object.at(key).is_number() || !std::isfinite(object.at(key).get<double>())) {
        fail("MALFORMED_JSON", path + "." + key,
             std::string(key) + " must be a finite number");
    }
    return object.at(key).get<double>();
}

bool boolean_value(const json& object, const char* key, const std::string& path, bool fallback) {
    if (!object.contains(key)) return fallback;
    if (!object.at(key).is_boolean()) {
        fail("MALFORMED_JSON", path + "." + key, std::string(key) + " must be a boolean");
    }
    return object.at(key).get<bool>();
}

// Cfd3dSolver::run() enforces these same bounds (128 x 128 x 256, product
// <=262144), but only after a caller has already allocated a dense field.
// validate_mesh_bounds() (declared in cfd3d_artifact_io.hpp so every loader
// can share it) is the earlier, and only reliable, enforcement point (Audit
// F2, issue #5).
constexpr int kMaximumMeshNx = 128;
constexpr int kMaximumMeshNy = 128;
constexpr int kMaximumMeshNz = 256;
constexpr std::uint64_t kMaximumMeshCells = 262144;

}  // namespace

void validate_mesh_bounds(const Cfd3dMesh& mesh, const std::string& path) {
    if (mesh.nx < 1 || mesh.ny < 1 || mesh.nz < 1) {
        fail("OUT_OF_RANGE", path, "mesh dimensions must be positive");
    }
    if (mesh.nx > kMaximumMeshNx) {
        fail("OUT_OF_RANGE", path, "mesh.nx must not exceed " + std::to_string(kMaximumMeshNx));
    }
    if (mesh.ny > kMaximumMeshNy) {
        fail("OUT_OF_RANGE", path, "mesh.ny must not exceed " + std::to_string(kMaximumMeshNy));
    }
    if (mesh.nz > kMaximumMeshNz) {
        fail("OUT_OF_RANGE", path, "mesh.nz must not exceed " + std::to_string(kMaximumMeshNz));
    }
    const auto nx = static_cast<std::uint64_t>(mesh.nx);
    const auto ny = static_cast<std::uint64_t>(mesh.ny);
    const auto nz = static_cast<std::uint64_t>(mesh.nz);
    if (nx > std::numeric_limits<std::uint64_t>::max() / ny ||
        nx * ny > std::numeric_limits<std::uint64_t>::max() / nz) {
        fail("OUT_OF_RANGE", path, "mesh dimensions overflow the field size");
    }
    const std::uint64_t count = nx * ny * nz;
    if (count > kMaximumMeshCells) {
        fail("OUT_OF_RANGE", path, "mesh cell product must not exceed " + std::to_string(kMaximumMeshCells));
    }
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fail("OUT_OF_RANGE", path, "mesh field is too large for this platform");
    }
}

namespace {

// checked_field_size() is the earlier, allocation-gating call site used
// throughout this file; it now delegates bound-checking to the public
// validate_mesh_bounds() so every caller (this loader and the CLI's
// workflows.cpp material loader) enforces the same limits.
std::size_t checked_field_size(const Cfd3dMesh& mesh, const std::string& path) {
    validate_mesh_bounds(mesh, path);
    return static_cast<std::size_t>(static_cast<std::uint64_t>(mesh.nx) * static_cast<std::uint64_t>(mesh.ny) *
                                     static_cast<std::uint64_t>(mesh.nz));
}

Cfd3dMaterialField parse_material(const json& value, const Cfd3dMesh& mesh,
                                   const std::string& path) {
    const std::size_t expected = checked_field_size(mesh, path);
    double uniform = 1.0;
    const json* values = nullptr;
    if (value.is_number()) {
        if (!std::isfinite(value.get<double>())) fail("MALFORMED_JSON", path, "material must be finite");
        uniform = value.get<double>();
    } else {
        require_object(value, path);
        if (value.contains("uniform")) {
            if (!value.at("uniform").is_number() ||
                !std::isfinite(value.at("uniform").get<double>())) {
                fail("MALFORMED_JSON", path + ".uniform", "uniform must be a finite number");
            }
            uniform = value.at("uniform").get<double>();
        }
        if (value.contains("values")) {
            if (!value.at("values").is_array()) {
                fail("MALFORMED_JSON", path + ".values", "values must be an array");
            }
            values = &value.at("values");
        }
    }

    if (values == nullptr) return Cfd3dMaterialField(mesh.nx, mesh.ny, mesh.nz, uniform);
    if (values->size() != expected) {
        fail("OUT_OF_RANGE", path + ".values", "material values must match mesh dimensions");
    }
    Cfd3dMaterialField material(mesh.nx, mesh.ny, mesh.nz, 1.0);
    for (std::size_t index = 0; index < values->size(); ++index) {
        if (!(*values)[index].is_number() || !std::isfinite((*values)[index].get<double>())) {
            fail("MALFORMED_JSON", path + ".values", "material values must be finite numbers");
        }
        const int x = static_cast<int>(index % static_cast<std::size_t>(mesh.nx));
        const std::size_t plane = static_cast<std::size_t>(mesh.nx) *
                                  static_cast<std::size_t>(mesh.ny);
        const int y = static_cast<int>((index / static_cast<std::size_t>(mesh.nx)) %
                                       static_cast<std::size_t>(mesh.ny));
        const int z = static_cast<int>(index / plane);
        material.at(x, y, z) = (*values)[index].get<double>();
    }
    return material;
}

json material_to_json(const Cfd3dMaterialField& material) {
    if (material.empty()) return json();
    json values = json::array();
    values.get_ref<json::array_t&>().reserve(material.values().size());
    for (const double value : material.values()) values.push_back(value);
    return json{{"values", std::move(values)}};
}

const char* warning_severity(WarningSeverity severity) {
    return severity == WarningSeverity::hard   ? "hard"
           : severity == WarningSeverity::soft ? "soft"
                                               : "info";
}

json warnings_to_json(const std::vector<SimulationWarning>& warnings) {
    json result = json::array();
    for (const auto& warning : warnings) {
        result.push_back({{"code", warning.code},
                          {"message", warning.message},
                          {"time_s", warning.time_s},
                          {"severity", warning_severity(warning.severity)}});
    }
    return result;
}

json field_descriptor(const char* name, const char* units) {
    return {{"name", name}, {"units", units}, {"scalar", "float64"}};
}

std::array<const char*, kFieldCount> field_names() {
    return {"pressure_pa", "saturation", "temperature_k", "pore_tds_fraction",
            "velocity_x_m_s", "velocity_y_m_s", "velocity_z_m_s"};
}

std::array<const char*, kFieldCount> field_units() {
    return {"Pa", "fraction", "K", "fraction", "m/s", "m/s", "m/s"};
}

std::array<const Cfd3dField*, kFieldCount> snapshot_fields(const Cfd3dSnapshot& snapshot) {
    return {&snapshot.pressure_pa,       &snapshot.saturation,       &snapshot.temperature_k,
            &snapshot.pore_tds_fraction, &snapshot.velocity_x_m_s, &snapshot.velocity_y_m_s,
            &snapshot.velocity_z_m_s};
}

std::array<const Cfd3dField*, kFieldCount> result_fields(const Cfd3dResult& result) {
    return {&result.pressure_pa,       &result.saturation,       &result.temperature_k,
            &result.pore_tds_fraction, &result.velocity_x_m_s, &result.velocity_y_m_s,
            &result.velocity_z_m_s};
}

void append_u64_le(std::string& output, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        output.push_back(static_cast<char>((value >> (byte * 8)) & 0xffU));
    }
}

void append_double_le(std::string& output, double value) {
    append_u64_le(output, std::bit_cast<std::uint64_t>(value));
}

void append_string(std::string& output, const std::string& value) {
    append_u64_le(output, static_cast<std::uint64_t>(value.size()));
    output.append(value);
}

void append_field(std::string& output, const Cfd3dField& field) {
    append_u64_le(output, static_cast<std::uint64_t>(field.size()));
    for (const double value : field.values()) append_double_le(output, value);
}

std::vector<std::uint8_t> field_bytes(const Cfd3dField& field) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(field.size() * sizeof(double));
    for (const double value : field.values()) {
        const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
        for (int byte = 0; byte < 8; ++byte) {
            bytes.push_back(static_cast<std::uint8_t>((bits >> (byte * 8)) & 0xffU));
        }
    }
    return bytes;
}

void write_u16(std::ostream& stream, std::uint16_t value) {
    const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xffU),
                                   static_cast<std::uint8_t>((value >> 8) & 0xffU)};
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void write_u32(std::ostream& stream, std::uint32_t value) {
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xffU),
                                   static_cast<std::uint8_t>((value >> 8) & 0xffU),
                                   static_cast<std::uint8_t>((value >> 16) & 0xffU),
                                   static_cast<std::uint8_t>((value >> 24) & 0xffU)};
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void write_u64(std::ostream& stream, std::uint64_t value) {
    std::uint8_t bytes[8]{};
    for (int byte = 0; byte < 8; ++byte) {
        bytes[byte] = static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffU);
    }
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void write_double(std::ostream& stream, double value) {
    write_u64(stream, std::bit_cast<std::uint64_t>(value));
}

std::uint64_t stream_offset(std::ostream& stream, const std::string& path) {
    const std::streampos position = stream.tellp();
    if (position < 0) fail("WRITE_FAILED", path, "could not determine field offset");
    return static_cast<std::uint64_t>(position);
}

void validate_snapshot_shape(const Cfd3dSnapshot& snapshot, const Cfd3dMesh& mesh,
                            const std::string& path) {
    const std::size_t expected = checked_field_size(mesh, path);
    for (const Cfd3dField* field : snapshot_fields(snapshot)) {
        if (field->x_cells() != mesh.nx || field->y_cells() != mesh.ny ||
            field->z_cells() != mesh.nz || field->size() != expected) {
            fail("OUT_OF_RANGE", path, "snapshot field dimensions do not match the mesh");
        }
    }
}

std::uint64_t snapshot_payload_bytes(const Cfd3dMesh& mesh, std::size_t snapshot_count) {
    const std::uint64_t field_values = static_cast<std::uint64_t>(checked_field_size(mesh, "cfd3d.mesh"));
    const std::uint64_t per_snapshot = field_values * static_cast<std::uint64_t>(kFieldCount) *
                                       static_cast<std::uint64_t>(sizeof(double));
    if (snapshot_count > 0 && per_snapshot > std::numeric_limits<std::uint64_t>::max() /
                                      static_cast<std::uint64_t>(snapshot_count)) {
        fail("OUT_OF_RANGE", "cfd3d.snapshots", "snapshot output size overflowed");
    }
    return per_snapshot * static_cast<std::uint64_t>(snapshot_count);
}

void validate_snapshots(const Cfd3dMesh& mesh, const std::vector<Cfd3dSnapshot>& snapshots) {
    if (snapshots.size() > kMaximumSnapshots) {
        fail("OUT_OF_RANGE", "cfd3d.snapshots", "snapshot count must not exceed 128");
    }
    const std::uint64_t bytes = snapshot_payload_bytes(mesh, snapshots.size());
    if (bytes > kMaximumFieldBytes) {
        fail("OUT_OF_RANGE", "cfd3d.snapshots", "snapshot output must not exceed 1 GiB");
    }
    double previous_time = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        validate_snapshot_shape(snapshots[index], mesh,
                                "cfd3d.snapshots[" + std::to_string(index) + "]");
        if (!std::isfinite(snapshots[index].time_s) || snapshots[index].time_s < previous_time) {
            fail("OUT_OF_RANGE", "cfd3d.snapshots", "snapshot times must be finite and ordered");
        }
        previous_time = snapshots[index].time_s;
    }
}

json mesh_to_json(const Cfd3dResult& result) {
    const Cfd3dGeometry& geometry = result.geometry;
    json classification = json::array();
    for (const Cfd3dCellClassification value : geometry.classification) {
        classification.push_back(value == Cfd3dCellClassification::outside       ? "outside"
                                 : value == Cfd3dCellClassification::inside     ? "inside"
                                 : value == Cfd3dCellClassification::cut        ? "cut"
                                                                                : "agglomerated");
    }
    return json{{"mesh", {{"nx", result.mesh.nx},
                           {"ny", result.mesh.ny},
                           {"nz", result.mesh.nz},
                           {"x_min_m", geometry.x_min_m},
                           {"y_min_m", geometry.y_min_m},
                           {"dx_m", geometry.dx_m},
                           {"dy_m", geometry.dy_m},
                           {"dz_m", geometry.dz_m},
                           {"cell_area_xy_m2", geometry.cell_area_xy_m2},
                           {"effective_cell_area_xy_m2", geometry.effective_cell_area_xy_m2},
                           {"x_face_aperture_m", geometry.x_face_aperture_m},
                           {"y_face_aperture_m", geometry.y_face_aperture_m},
                           {"agglomerate_parent", geometry.agglomerate_parent},
                           {"classification", std::move(classification)}}}};
}

void write_fields(const std::filesystem::path& directory, const Cfd3dResult& result,
                  const std::vector<Cfd3dSnapshot>& snapshots,
                  Cfd3dRunManifest& manifest) {
    validate_snapshots(result.mesh, snapshots);
    const std::filesystem::path field_path = directory / "fields.elf3d";
    std::ofstream stream(field_path, std::ios::binary | std::ios::trunc);
    if (!stream) fail("WRITE_FAILED", field_path.string(), "could not write field container");

    const auto names = field_names();
    const auto units = field_units();
    json fields = json::array();
    for (std::size_t index = 0; index < kFieldCount; ++index) {
        fields.push_back(field_descriptor(names[index], units[index]));
    }
    const json metadata = {{"format", std::string(version::kCfd3dFieldFormat)},
                           {"byte_order", "little-endian"},
                           {"scalar_type", "IEEE-754 float64"},
                           {"ordering", "x-fastest, then y, then z"},
                           {"mesh", { {"nx", result.mesh.nx},
                                       {"ny", result.mesh.ny},
                                       {"nz", result.mesh.nz},
                                       {"x_min_m", result.geometry.x_min_m},
                                       {"y_min_m", result.geometry.y_min_m},
                                       {"dx_m", result.geometry.dx_m},
                                       {"dy_m", result.geometry.dy_m},
                                       {"dz_m", result.geometry.dz_m}}},
                           {"fields", std::move(fields)}};
    const std::string metadata_text = metadata.dump();
    if (metadata_text.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("OUT_OF_RANGE", field_path.string(), "field metadata is too large");
    }
    const char magic[8] = {'E', 'L', 'F', '3', 'D', '\0', '\1', '\0'};
    stream.write(magic, sizeof(magic));
    write_u16(stream, 1);
    stream.put(static_cast<char>(1));  // little-endian marker
    stream.put(static_cast<char>(2));  // float64 marker
    write_u32(stream, static_cast<std::uint32_t>(metadata_text.size()));
    stream.write(metadata_text.data(), static_cast<std::streamsize>(metadata_text.size()));

    json chunks = json::array();
    std::uint64_t field_bytes_written = 0;
    for (std::size_t snapshot_index = 0; snapshot_index < snapshots.size(); ++snapshot_index) {
        const auto fields_at_time = snapshot_fields(snapshots[snapshot_index]);
        for (std::size_t field_index = 0; field_index < fields_at_time.size(); ++field_index) {
            const std::vector<std::uint8_t> bytes = field_bytes(*fields_at_time[field_index]);
            const std::string digest = artifact_io::sha256_hex(
                std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            const std::uint64_t chunk_offset = stream_offset(stream, field_path.string());
            write_u32(stream, static_cast<std::uint32_t>(snapshot_index));
            write_u32(stream, static_cast<std::uint32_t>(field_index));
            write_double(stream, snapshots[snapshot_index].time_s);
            write_u64(stream, static_cast<std::uint64_t>(fields_at_time[field_index]->size()));
            write_u64(stream, static_cast<std::uint64_t>(bytes.size()));
            for (std::size_t byte = 0; byte < digest.size(); byte += 2) {
                const auto hex_value = [](char digit) -> std::uint8_t {
                    if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
                    if (digit >= 'a' && digit <= 'f') {
                        return static_cast<std::uint8_t>(digit - 'a' + 10);
                    }
                    return static_cast<std::uint8_t>(digit - 'A' + 10);
                };
                const std::uint8_t value = static_cast<std::uint8_t>(
                    (hex_value(digest[byte]) << 4U) | hex_value(digest[byte + 1]));
                stream.put(static_cast<char>(value));
            }
            const std::uint64_t payload_offset = stream_offset(stream, field_path.string());
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            if (!stream) fail("WRITE_FAILED", field_path.string(), "could not write field payload");
            field_bytes_written += static_cast<std::uint64_t>(bytes.size());
            chunks.push_back({{"snapshot_index", snapshot_index},
                              {"time_s", snapshots[snapshot_index].time_s},
                              {"field", names[field_index]},
                              {"chunk_offset", chunk_offset},
                              {"payload_offset", payload_offset},
                              {"payload_bytes", bytes.size()},
                              {"sha256", digest}});
        }
    }
    if (field_bytes_written != snapshot_payload_bytes(result.mesh, snapshots.size())) {
        fail("WRITE_FAILED", field_path.string(), "field payload size did not match the mesh");
    }
    manifest.field_bytes = field_bytes_written;
    write_file(directory / "index.json",
               json{{"format", std::string(version::kCfd3dFieldFormat)},
                    {"snapshot_count", snapshots.size()},
                    {"field_count", kFieldCount},
                    {"chunk_header_bytes", kChunkHeaderBytes},
                    {"chunks", std::move(chunks)}}
                   .dump(2));
}

}  // namespace

Cfd3dCase load_case_json(const std::string& json_text) {
    const json root = parse_json(json_text, "cfd3d");
    require_object(root, "cfd3d");
    // Audit F4, issue #6: root.value("schema_version", ...) only tolerates a
    // *missing* key -- a present key of the wrong type (e.g. a number) still
    // throws nlohmann::json::type_error, which reached the CLI/server as an
    // uncaught INTERNAL_ERROR instead of a structured MALFORMED_JSON.
    if (root.contains("schema_version") && !root.at("schema_version").is_string()) {
        fail("MALFORMED_JSON", "cfd3d.schema_version", "schema_version must be a string");
    }
    const std::string schema = root.value("schema_version", std::string(version::kCfd3dCaseSchema));
    if (schema != version::kCfd3dCaseSchema) {
        fail("UNSUPPORTED_SCHEMA_VERSION", "cfd3d.schema_version",
             "cfd3d schema_version '" + schema + "' is not supported");
    }
    if (!root.contains("recipe")) fail("MISSING_FIELD", "cfd3d.recipe", "recipe is required");

    // Every field below this point already goes through a type-checked
    // helper (require_object/integer_value/number_value/boolean_value) or a
    // nested loader that raises LoadError itself. This catch is defense in
    // depth: it guarantees any other malformed field anywhere in this
    // document -- including inside the nested recipe/coefficients JSON --
    // still surfaces as a stable MALFORMED_JSON rather than an uncaught
    // nlohmann exception (LoadError itself is not a json::exception, so it
    // passes through this catch unchanged).
    try {
        Cfd3dCase result;
        result.recipe = artifact_io::load_recipe_json(root.at("recipe").dump());
        if (root.contains("coefficients")) {
            result.coefficients = artifact_io::load_coefficients_json(root.at("coefficients").dump());
        }
        if (root.contains("mesh")) {
            require_object(root.at("mesh"), "cfd3d.mesh");
            const json& mesh = root.at("mesh");
            result.config.mesh.nx = integer_value(mesh, "nx", "cfd3d.mesh", result.config.mesh.nx);
            result.config.mesh.ny = integer_value(mesh, "ny", "cfd3d.mesh", result.config.mesh.ny);
            result.config.mesh.nz = integer_value(mesh, "nz", "cfd3d.mesh", result.config.mesh.nz);
        }
        if (root.contains("solver")) {
            require_object(root.at("solver"), "cfd3d.solver");
            const json& solver = root.at("solver");
            result.config.dt_s = number_value(solver, "dt_s", "cfd3d.solver", result.config.dt_s);
            result.config.sample_interval_s =
                number_value(solver, "sample_interval_s", "cfd3d.solver", result.config.sample_interval_s);
            result.config.cfl_number =
                number_value(solver, "cfl_number", "cfd3d.solver", result.config.cfl_number);
            result.config.pressure_tolerance =
                number_value(solver, "pressure_tolerance", "cfd3d.solver", result.config.pressure_tolerance);
            result.config.pressure_max_iterations = integer_value(
                solver, "pressure_max_iterations", "cfd3d.solver", result.config.pressure_max_iterations);
            result.config.snapshot_interval_s =
                number_value(solver, "snapshot_interval_s", "cfd3d.solver", result.config.snapshot_interval_s);
            result.config.snapshot_initial =
                boolean_value(solver, "snapshot_initial", "cfd3d.solver", result.config.snapshot_initial);
            result.config.snapshot_final =
                boolean_value(solver, "snapshot_final", "cfd3d.solver", result.config.snapshot_final);
        }
        if (root.contains("material")) {
            result.config.material = parse_material(root.at("material"), result.config.mesh, "cfd3d.material");
        }
        return result;
    } catch (const json::exception& e) {
        fail("MALFORMED_JSON", "cfd3d", e.what());
    }
}

Cfd3dCase load_case_file(const std::filesystem::path& file) {
    return load_case_json(read_file(file));
}

std::string dump_case_json(const Cfd3dCase& cfd3d_case, int indent) {
    json root = {{"schema_version", std::string(version::kCfd3dCaseSchema)},
                 {"recipe", json::parse(artifact_io::dump_recipe_json(cfd3d_case.recipe, -1))},
                 {"coefficients",
                  json::parse(artifact_io::dump_coefficients_json(cfd3d_case.coefficients, -1))},
                 {"mesh", {{"nx", cfd3d_case.config.mesh.nx},
                            {"ny", cfd3d_case.config.mesh.ny},
                            {"nz", cfd3d_case.config.mesh.nz}}},
                 {"solver", {{"dt_s", cfd3d_case.config.dt_s},
                              {"sample_interval_s", cfd3d_case.config.sample_interval_s},
                              {"cfl_number", cfd3d_case.config.cfl_number},
                              {"pressure_tolerance", cfd3d_case.config.pressure_tolerance},
                              {"pressure_max_iterations",
                               cfd3d_case.config.pressure_max_iterations},
                              {"snapshot_interval_s", cfd3d_case.config.snapshot_interval_s},
                              {"snapshot_initial", cfd3d_case.config.snapshot_initial},
                              {"snapshot_final", cfd3d_case.config.snapshot_final}}}};
    if (!cfd3d_case.config.material.empty()) root["material"] = material_to_json(cfd3d_case.config.material);
    return root.dump(indent);
}

std::string result_hash(const Cfd3dCase& cfd3d_case, const Cfd3dResult& result,
                        const std::vector<Cfd3dSnapshot>& snapshots) {
    validate_snapshots(result.mesh, snapshots);
    std::string bytes;
    append_string(bytes, std::string(version::kCfd3dResultSchema));
    append_string(bytes, std::string(version::kCfd3dFieldFormat));
    append_string(bytes, dump_case_json(cfd3d_case, -1));
    append_string(bytes, result.solver_version);
    append_double_le(bytes, result.elapsed_time_s);
    append_double_le(bytes, result.beverage_mass_kg);
    append_double_le(bytes, result.tds_fraction);
    append_double_le(bytes, result.extraction_yield_fraction);
    for (const auto* field : result_fields(result)) append_field(bytes, *field);
    for (const auto& sample : result.samples) {
        append_double_le(bytes, sample.time_s);
        append_double_le(bytes, sample.pressure_pa);
        append_double_le(bytes, sample.inlet_temperature_k);
        append_double_le(bytes, sample.puck_temperature_k);
        append_double_le(bytes, sample.flow_m3_s);
        append_double_le(bytes, sample.beverage_mass_kg);
        append_double_le(bytes, sample.tds_fraction);
        append_double_le(bytes, sample.extraction_yield_fraction);
        append_double_le(bytes, sample.saturation);
        append_double_le(bytes, sample.permeability_m2);
    }
    for (const auto& snapshot : snapshots) {
        append_double_le(bytes, snapshot.time_s);
        for (const auto* field : snapshot_fields(snapshot)) append_field(bytes, *field);
    }
    return artifact_io::sha256_hex(bytes);
}

Cfd3dRunManifest make_manifest(const Cfd3dCase& cfd3d_case, const Cfd3dResult& result,
                               const std::vector<Cfd3dSnapshot>& snapshots) {
    Cfd3dRunManifest manifest;
    manifest.case_schema_version = std::string(version::kCfd3dCaseSchema);
    manifest.result_schema_version = std::string(version::kCfd3dResultSchema);
    manifest.field_format = std::string(version::kCfd3dFieldFormat);
    manifest.solver_version = result.solver_version;
    manifest.recipe_hash = artifact_io::recipe_hash(cfd3d_case.recipe);
    manifest.coefficient_hash = artifact_io::coefficient_hash(cfd3d_case.coefficients);
    manifest.result_hash = result_hash(cfd3d_case, result, snapshots);
    manifest.run_id = "cfd3d-" + manifest.result_hash.substr(0, 12);
    manifest.dt_s = cfd3d_case.config.dt_s;
    manifest.sample_interval_s = cfd3d_case.config.sample_interval_s;
    manifest.snapshot_interval_s = cfd3d_case.config.snapshot_interval_s;
    manifest.snapshot_count = snapshots.size();
    manifest.field_bytes = snapshot_payload_bytes(result.mesh, snapshots.size());
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    manifest.timestamp_utc = std::format("{:%Y-%m-%dT%H:%M:%SZ}", seconds);
    return manifest;
}

std::string dump_manifest_json(const Cfd3dRunManifest& manifest, int indent) {
    return json{{"run_id", manifest.run_id},
                {"case_schema_version", manifest.case_schema_version},
                {"result_schema_version", manifest.result_schema_version},
                {"field_format", manifest.field_format},
                {"solver_version", manifest.solver_version},
                {"recipe_hash", manifest.recipe_hash},
                {"coefficient_hash", manifest.coefficient_hash},
                {"result_hash", manifest.result_hash},
                {"timestamp_utc", manifest.timestamp_utc},
                {"dt_s", manifest.dt_s},
                {"sample_interval_s", manifest.sample_interval_s},
                {"snapshot_interval_s", manifest.snapshot_interval_s},
                {"snapshot_count", manifest.snapshot_count},
                {"field_bytes", manifest.field_bytes}}
        .dump(indent);
}

std::string dump_summary_json(const Cfd3dResult& result, int indent) {
    const Cfd3dDiagnostics& diagnostics = result.diagnostics;
    json root = {{"termination", to_string(result.termination)},
                 {"elapsed_time_s", result.elapsed_time_s},
                 {"beverage_mass_g", units::kg_to_grams(result.beverage_mass_kg)},
                 {"tds_percent", result.tds_fraction * 100.0},
                 {"extraction_yield_percent", result.extraction_yield_fraction * 100.0},
                 {"mesh", {{"nx", result.mesh.nx}, {"ny", result.mesh.ny}, {"nz", result.mesh.nz}}},
                 {"diagnostics", {{"max_total_velocity_divergence_1_s",
                                    diagnostics.max_total_velocity_divergence_1_s},
                                   {"pressure_residual", diagnostics.pressure_residual},
                                   {"pressure_iterations_total", diagnostics.pressure_iterations_total},
                                   {"step_count", diagnostics.step_count},
                                   {"water_mass_residual_g",
                                    units::kg_to_grams(diagnostics.water_mass_residual_kg)},
                                   {"solids_mass_residual_g",
                                    units::kg_to_grams(diagnostics.solids_mass_residual_kg)},
                                   {"energy_residual_j", diagnostics.energy_residual_j},
                                   {"max_courant_number", diagnostics.max_courant_number},
                                   {"saturation_clamp_count", diagnostics.saturation_clamp_count},
                                   {"nonfinite_state_count", diagnostics.nonfinite_state_count},
                                   {"agglomerated_sliver_count",
                                    diagnostics.agglomerated_sliver_count}}},
                 {"warnings", warnings_to_json(result.warnings)}};
    return root.dump(indent);
}

std::string dump_samples_csv(const Cfd3dResult& result) {
    std::ostringstream output;
    output << "time_s,pressure_bar,inlet_temperature_c,puck_temperature_c,flow_ml_s,"
              "beverage_mass_g,tds_percent,extraction_yield_percent,saturation,permeability_m2\n";
    output << std::setprecision(17);
    for (const auto& sample : result.samples) {
        output << sample.time_s << ',' << units::pa_to_bar(sample.pressure_pa) << ','
               << units::kelvin_to_celsius(sample.inlet_temperature_k) << ','
               << units::kelvin_to_celsius(sample.puck_temperature_k) << ','
               << units::m3_s_to_ml_s(sample.flow_m3_s) << ','
               << units::kg_to_grams(sample.beverage_mass_kg) << ','
               << sample.tds_fraction * 100.0 << ','
               << sample.extraction_yield_fraction * 100.0 << ',' << sample.saturation << ','
               << sample.permeability_m2 << '\n';
    }
    return output.str();
}

void write_artifacts(const std::filesystem::path& directory, const Cfd3dCase& cfd3d_case,
                     const Cfd3dResult& result,
                     const std::vector<Cfd3dSnapshot>& snapshots) {
    validate_snapshots(result.mesh, snapshots);
    std::filesystem::create_directories(directory);
    write_file(directory / "case.json", dump_case_json(cfd3d_case));
    write_file(directory / "recipe.json", artifact_io::dump_recipe_json(cfd3d_case.recipe));
    write_file(directory / "coefficients.json",
               artifact_io::dump_coefficients_json(cfd3d_case.coefficients));
    write_file(directory / "mesh.json", mesh_to_json(result).dump(2));
    write_file(directory / "summary.json", dump_summary_json(result));
    write_file(directory / "samples.csv", dump_samples_csv(result));
    Cfd3dRunManifest manifest = make_manifest(cfd3d_case, result, snapshots);
    write_fields(directory, result, snapshots, manifest);
    write_file(directory / "manifest.json", dump_manifest_json(manifest));
}

}  // namespace espressolab::cfd3d_artifact_io
