#include <catch_amalgamated.hpp>
#include <string>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/bean.hpp"

using namespace espressolab;
using Catch::Matchers::Predicate;

namespace {

// A minimal well-formed bean, so each test can break exactly one thing.
std::string valid_bean_text(const std::string& mutation = "") {
    std::string text = R"({
      "id": "test-bean",
      "classes": {
        "acids":       { "mass_fraction": 0.15 },
        "sugars":      { "mass_fraction": 0.30 },
        "maillard":    { "mass_fraction": 0.30 },
        "lipids":      { "mass_fraction": 0.10 },
        "bitter":      { "mass_fraction": 0.10 },
        "polyphenols": { "mass_fraction": 0.05 }
      },
      "target": {
        "fruit":       { "intensity": 5.0 },
        "acidity":     { "intensity": 5.0 },
        "sweetness":   { "intensity": 5.0 },
        "chocolate":   { "intensity": 5.0 },
        "body":        { "intensity": 5.0 },
        "bitterness":  { "intensity": 3.0 },
        "astringency": { "intensity": 2.0 }
      }
    })";
    if (!mutation.empty()) {
        nlohmann::json document = nlohmann::json::parse(text);
        document.merge_patch(nlohmann::json::parse(mutation));
        return document.dump();
    }
    return text;
}

auto load_error_is(const std::string& code, const std::string& path) {
    return Predicate<artifact_io::LoadError>([code, path](const artifact_io::LoadError& e) {
        return e.code == code && e.path == path;
    });
}

}  // namespace

TEST_CASE("every shipped bean document loads and validates", "[flavor][unit]") {
    for (const char* id : {"counter-culture-hologram", "ethiopia-dhiba-bate-natural-light",
                           "continental-dark-roast"}) {
        const BeanProfile bean = artifact_io::load_bean_file(
            testing::asset_dir() / "beans" / (std::string(id) + ".json"));
        INFO("bean: " << id);
        REQUIRE(bean.id == id);
        REQUIRE(bean.validate().ok());
        REQUIRE(bean.description.has_value());
        // Each shipped document has to carry its own caveat: these files outlive
        // the repository that explains them.
        REQUIRE_FALSE(bean.description->limitations.empty());
    }
}

TEST_CASE("a bean survives a round trip through a recipe", "[flavor][unit][artifacts]") {
    const Recipe original = testing::hologram_recipe();
    const Recipe reloaded = artifact_io::load_recipe_json(artifact_io::dump_recipe_json(original));

    REQUIRE(reloaded.bean.has_value());
    REQUIRE(reloaded.bean->id == original.bean->id);
    REQUIRE(reloaded.bean->version == original.bean->version);
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        REQUIRE(reloaded.bean->classes[k].mass_fraction ==
                original.bean->classes[k].mass_fraction);
        REQUIRE(reloaded.bean->classes[k].relative_rate ==
                original.bean->classes[k].relative_rate);
    }
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        REQUIRE(reloaded.bean->target[a].intensity == original.bean->target[a].intensity);
        for (std::size_t c = 0; c < kSoluteClassCount; ++c) {
            REQUIRE(reloaded.bean->axis_weights[a][c] == original.bean->axis_weights[a][c]);
        }
    }
    REQUIRE(reloaded.bean->description->roaster == original.bean->description->roaster);
    REQUIRE(reloaded.bean->description->notes == original.bean->description->notes);
}

TEST_CASE("a recipe without a bean serializes no bean key", "[flavor][unit][artifacts]") {
    const nlohmann::json document =
        nlohmann::json::parse(artifact_io::dump_recipe_json(testing::baseline_recipe()));
    // The omission is the whole reason every pre-overlay recipe keeps its hash.
    REQUIRE_FALSE(document.contains("bean"));
}

TEST_CASE("malformed bean documents name their own path", "[flavor][unit][artifacts]") {
    SECTION("an unknown solute class is rejected, not ignored") {
        REQUIRE_THROWS_MATCHES(
            artifact_io::load_bean_json(valid_bean_text(R"({"classes":{"esters":{"mass_fraction":0}}})")),
            artifact_io::LoadError, load_error_is("UNKNOWN_SOLUTE_CLASS", "bean.classes.esters"));
    }
    SECTION("an unknown sensory axis is rejected, not ignored") {
        REQUIRE_THROWS_MATCHES(
            artifact_io::load_bean_json(valid_bean_text(R"({"target":{"umami":{"intensity":1}}})")),
            artifact_io::LoadError, load_error_is("UNKNOWN_SENSORY_AXIS", "bean.target.umami"));
    }
    SECTION("a missing solute class is a missing field") {
        nlohmann::json document = nlohmann::json::parse(valid_bean_text());
        document["classes"].erase("lipids");
        REQUIRE_THROWS_MATCHES(artifact_io::load_bean_json(document.dump()),
                               artifact_io::LoadError,
                               load_error_is("MISSING_FIELD", "bean.classes.lipids"));
    }
    SECTION("a missing mass fraction names the class it belongs to") {
        nlohmann::json document = nlohmann::json::parse(valid_bean_text());
        document["classes"]["sugars"] = nlohmann::json::object();
        REQUIRE_THROWS_MATCHES(
            artifact_io::load_bean_json(document.dump()), artifact_io::LoadError,
            load_error_is("MISSING_FIELD", "bean.classes.sugars.mass_fraction"));
    }
    SECTION("an unsupported schema version is refused") {
        REQUIRE_THROWS_MATCHES(
            artifact_io::load_bean_json(valid_bean_text(R"({"schema_version":"2.0"})")),
            artifact_io::LoadError,
            load_error_is("UNSUPPORTED_SCHEMA_VERSION", "bean.schema_version"));
    }
}

TEST_CASE("bean validation catches what the loader cannot", "[flavor][unit]") {
    const auto issue_paths = [](const ValidationResult& result) {
        std::vector<std::string> paths;
        for (const ValidationIssue& issue : result.issues()) paths.push_back(issue.path);
        return paths;
    };

    SECTION("class mass fractions must partition the extractable solids") {
        BeanProfile bean = testing::hologram_bean();
        bean.classes[0].mass_fraction += 0.05;  // now sums to 1.05
        const ValidationResult result = bean.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE_THAT(issue_paths(result),
                     Catch::Matchers::VectorContains(std::string("recipe.bean.classes")));
    }
    SECTION("a relative rate of zero would freeze a class out entirely") {
        BeanProfile bean = testing::hologram_bean();
        bean.classes[2].relative_rate = 0.0;
        REQUIRE_FALSE(bean.validate().ok());
    }
    SECTION("an all-zero weight row has no shape to normalise") {
        BeanProfile bean = testing::hologram_bean();
        bean.axis_weights[static_cast<std::size_t>(SensoryAxis::body)] = {};
        const ValidationResult result = bean.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE_THAT(issue_paths(result),
                     Catch::Matchers::VectorContains(std::string("recipe.bean.axis_weights.body")));
    }
    SECTION("a target with no weighted axis cannot be scored against") {
        BeanProfile bean = testing::hologram_bean();
        for (std::size_t a = 0; a < kSensoryAxisCount; ++a) bean.target[a].weight = 0.0;
        REQUIRE_FALSE(bean.validate().ok());
    }
    SECTION("a bad bean is reported through the recipe that carries it") {
        Recipe recipe = testing::hologram_recipe();
        recipe.bean->target[0].intensity = 99.0;
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE_THAT(issue_paths(result),
                     Catch::Matchers::VectorContains(std::string("recipe.bean.target.fruit.intensity")));
    }
}

TEST_CASE("a bean description is metadata, not identity", "[flavor][unit][artifacts]") {
    const Recipe original = testing::hologram_recipe();
    const std::string baseline_hash = artifact_io::recipe_hash(original);

    SECTION("editing the prose leaves the hash alone") {
        Recipe edited = original;
        edited.bean->description->notes = {"typo fixed", "still the same coffee"};
        edited.bean->description->roaster = "Counter Culture Coffee (Durham, NC)";
        edited.bean->description->limitations.push_back("an added caveat");
        // Same rule as coefficient provenance: fixing a tasting note must not
        // change what the run means or where its artifacts land.
        REQUIRE(artifact_io::recipe_hash(edited) == baseline_hash);
    }
    SECTION("dropping the description entirely leaves the hash alone") {
        Recipe edited = original;
        edited.bean->description.reset();
        REQUIRE(artifact_io::recipe_hash(edited) == baseline_hash);
    }
    SECTION("editing a class share moves the hash") {
        Recipe edited = original;
        edited.bean->classes[0].mass_fraction += 0.01;
        edited.bean->classes[1].mass_fraction -= 0.01;
        REQUIRE(artifact_io::recipe_hash(edited) != baseline_hash);
    }
    SECTION("editing a target moves the hash") {
        Recipe edited = original;
        edited.bean->target[0].intensity += 0.5;
        REQUIRE(artifact_io::recipe_hash(edited) != baseline_hash);
    }
}
