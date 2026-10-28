#pragma once

#include "EngineExport.h"
#include "Assets/AssetCompiler.hpp"
#include "Project/PackageDependency.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ENGINE_API PackageReceipt
{
    uint32_t schema_version = 1;
    std::string hash_algorithm = "fnv1a64";
    std::string package_name;
    std::string project_name;
    std::string engine_version;
    std::string default_level;
    std::string game_module;
    Assets::CompileConfig compile_config;
    std::vector<PackageDependency> dependencies;
    uint64_t closure_hash_fnv1a64 = 0;
};

struct ENGINE_API PackageReceiptVerification
{
    bool valid = false;
    std::vector<std::string> errors;
};

// Serialization and verification operate only on a staged/final package root.
// They have no source-project, renderer, audio, or ECS dependencies.
class ENGINE_API PackageReceiptSerializer
{
public:
    static constexpr uint32_t SchemaVersion = 1;
    static constexpr const char* Filename = "package_receipt.json";

    // Canonicalizes dependency order, validates receipt fields, and sets the
    // closure hash. Callers receive stable errors sorted bytewise.
    static bool canonicalize(PackageReceipt& receipt, std::vector<std::string>* errors_out = nullptr);

    // Writes <staged_package_root>/package_receipt.json through a sibling temp
    // file and atomic file replacement. The root must already be a private
    // staging directory owned by the caller.
    static bool writeStaged(PackageReceipt receipt,
                            const std::string& staged_package_root,
                            std::string* error_out = nullptr);

    static bool readStaged(const std::string& package_root,
                           PackageReceipt& receipt,
                           std::string* error_out = nullptr);

    // Reopens the receipt and verifies every dependency byte count and staged
    // artifact FNV value. Source hashes are provenance and are not read here.
    static PackageReceiptVerification verifyStaged(const std::string& package_root);

    static std::string formatHash(uint64_t value);
    static bool parseHash(const std::string& value, uint64_t& output);
};