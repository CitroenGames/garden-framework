#include "PackageReceipt.hpp"

#include "Utils/FileHash.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
std::string makeError(const std::string& code, const std::string& detail)
{
    return code + ": " + detail;
}

void sortErrors(std::vector<std::string>& errors)
{
    std::sort(errors.begin(), errors.end());
    errors.erase(std::unique(errors.begin(), errors.end()), errors.end());
}

bool receiptDependencyLess(const PackageDependency& lhs, const PackageDependency& rhs)
{
    if (lhs.logical_path != rhs.logical_path) return lhs.logical_path < rhs.logical_path;
    if (lhs.kind != rhs.kind) return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);
    return lhs.owner_logical_path < rhs.owner_logical_path;
}

bool sameReceiptDependency(const PackageDependency& lhs, const PackageDependency& rhs)
{
    return lhs.logical_path == rhs.logical_path && lhs.kind == rhs.kind;
}

json compileConfigJson(const Assets::CompileConfig& config)
{
    return {
        {"animation_cooker_version", config.animation_cooker_version},
        {"bc7_quality", config.bc7_quality},
        {"compile_animations", config.compile_animations},
        {"compile_models", config.compile_models},
        {"compile_textures", config.compile_textures},
        {"cooker_version", config.cooker_version},
        {"default_color_format", static_cast<uint32_t>(config.default_color_format)},
        {"generate_mipmaps", config.generate_mipmaps},
        {"incremental", config.incremental},
        {"normal_map_format", static_cast<uint32_t>(config.normal_map_format)},
    };
}

bool parseCompileConfig(const json& value, Assets::CompileConfig& config, std::string& error)
{
    static constexpr std::array<const char*, 10> fields = {
        "animation_cooker_version", "bc7_quality", "compile_animations", "compile_models",
        "compile_textures", "cooker_version", "default_color_format", "generate_mipmaps",
        "incremental", "normal_map_format"
    };
    if (!value.is_object())
    {
        error = "compile_config must be an object";
        return false;
    }
    for (const char* field : fields)
    {
        if (!value.contains(field))
        {
            error = std::string("compile_config is missing ") + field;
            return false;
        }
    }
    if (!value["compile_models"].is_boolean() || !value["compile_textures"].is_boolean() ||
        !value["compile_animations"].is_boolean() || !value["incremental"].is_boolean() ||
        !value["generate_mipmaps"].is_boolean() || !value["bc7_quality"].is_number_integer() ||
        !value["cooker_version"].is_number_unsigned() || !value["animation_cooker_version"].is_number_unsigned() ||
        !value["default_color_format"].is_number_unsigned() || !value["normal_map_format"].is_number_unsigned())
    {
        error = "compile_config contains a field with the wrong type";
        return false;
    }

    const uint32_t default_format = value["default_color_format"].get<uint32_t>();
    const uint32_t normal_format = value["normal_map_format"].get<uint32_t>();
    if (default_format > static_cast<uint32_t>(Assets::TexCompressionFormat::BC7) ||
        normal_format > static_cast<uint32_t>(Assets::TexCompressionFormat::BC7))
    {
        error = "compile_config contains an unknown texture compression format";
        return false;
    }

    config.compile_models = value["compile_models"].get<bool>();
    config.compile_textures = value["compile_textures"].get<bool>();
    config.compile_animations = value["compile_animations"].get<bool>();
    config.incremental = value["incremental"].get<bool>();
    config.generate_mipmaps = value["generate_mipmaps"].get<bool>();
    config.bc7_quality = value["bc7_quality"].get<int>();
    config.cooker_version = value["cooker_version"].get<uint32_t>();
    config.animation_cooker_version = value["animation_cooker_version"].get<uint32_t>();
    config.default_color_format = static_cast<Assets::TexCompressionFormat>(default_format);
    config.normal_map_format = static_cast<Assets::TexCompressionFormat>(normal_format);
    config.inject_publish_failure = false;
    return true;
}

json dependencyJson(const PackageDependency& dependency)
{
    return {
        {"byte_size", dependency.byte_size},
        {"cook_fingerprint", PackageReceiptSerializer::formatHash(dependency.cook_fingerprint)},
        {"cooked_hash_fnv1a64", PackageReceiptSerializer::formatHash(dependency.cooked_hash_fnv1a64)},
        {"extractor_id", dependency.extractor_id},
        {"format_version", dependency.format_version},
        {"kind", packageDependencyKindName(dependency.kind)},
        {"logical_path", dependency.logical_path},
        {"owner_logical_path", dependency.owner_logical_path},
        {"required_at_runtime", dependency.required_at_runtime},
        {"source_hash_fnv1a64", PackageReceiptSerializer::formatHash(dependency.source_hash_fnv1a64)},
        {"source_logical_path", dependency.source_logical_path},
    };
}

bool parseDependency(const json& value, PackageDependency& dependency, std::string& error)
{
    static constexpr std::array<const char*, 11> fields = {
        "byte_size", "cook_fingerprint", "cooked_hash_fnv1a64", "extractor_id", "format_version", "kind",
        "logical_path", "owner_logical_path", "required_at_runtime", "source_hash_fnv1a64", "source_logical_path"
    };
    if (!value.is_object())
    {
        error = "dependency must be an object";
        return false;
    }
    for (const char* field : fields)
    {
        if (!value.contains(field))
        {
            error = std::string("dependency is missing ") + field;
            return false;
        }
    }
    if (!value["logical_path"].is_string() || !value["kind"].is_string() ||
        !value["owner_logical_path"].is_string() || !value["extractor_id"].is_string() ||
        !value["source_logical_path"].is_string() || !value["source_hash_fnv1a64"].is_string() ||
        !value["cooked_hash_fnv1a64"].is_string() || !value["cook_fingerprint"].is_string() ||
        !value["format_version"].is_number_unsigned() || !value["byte_size"].is_number_unsigned() ||
        !value["required_at_runtime"].is_boolean())
    {
        error = "dependency contains a field with the wrong type";
        return false;
    }
    dependency.logical_path = value["logical_path"].get<std::string>();
    dependency.owner_logical_path = value["owner_logical_path"].get<std::string>();
    dependency.extractor_id = value["extractor_id"].get<std::string>();
    dependency.source_logical_path = value["source_logical_path"].get<std::string>();
    dependency.format_version = value["format_version"].get<uint32_t>();
    dependency.byte_size = value["byte_size"].get<uint64_t>();
    dependency.required_at_runtime = value["required_at_runtime"].get<bool>();
    if (!parsePackageDependencyKind(value["kind"].get<std::string>(), dependency.kind) ||
        !PackageReceiptSerializer::parseHash(value["source_hash_fnv1a64"].get<std::string>(), dependency.source_hash_fnv1a64) ||
        !PackageReceiptSerializer::parseHash(value["cooked_hash_fnv1a64"].get<std::string>(), dependency.cooked_hash_fnv1a64) ||
        !PackageReceiptSerializer::parseHash(value["cook_fingerprint"].get<std::string>(), dependency.cook_fingerprint))
    {
        error = "dependency contains an unknown kind or invalid fixed-width hash";
        return false;
    }
    return true;
}

json receiptJson(const PackageReceipt& receipt, bool include_closure_hash)
{
    json dependencies = json::array();
    for (const PackageDependency& dependency : receipt.dependencies)
        dependencies.push_back(dependencyJson(dependency));

    json document = {
        {"compile_config", compileConfigJson(receipt.compile_config)},
        {"default_level", receipt.default_level},
        {"dependencies", dependencies},
        {"engine_version", receipt.engine_version},
        {"game_module", receipt.game_module},
        {"hash_algorithm", receipt.hash_algorithm},
        {"package_name", receipt.package_name},
        {"project_name", receipt.project_name},
        {"schema_version", receipt.schema_version},
    };
    if (include_closure_hash)
        document["closure_hash_fnv1a64"] = PackageReceiptSerializer::formatHash(receipt.closure_hash_fnv1a64);
    return document;
}

uint64_t closureHash(const PackageReceipt& receipt)
{
    const std::string bytes = receiptJson(receipt, false).dump();
    return Utils::hashBuffer(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

bool atomicReplaceFile(const fs::path& temporary_path, const fs::path& final_path)
{
#ifdef _WIN32
    if (MoveFileExA(temporary_path.string().c_str(), final_path.string().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return true;
#else
    std::error_code ec;
    fs::rename(temporary_path, final_path, ec);
    if (!ec) return true;
#endif
    std::error_code ignored;
    fs::remove(temporary_path, ignored);
    return false;
}
}

std::string PackageReceiptSerializer::formatHash(uint64_t value)
{
    std::array<char, 19> chars{};
    chars[0] = '0';
    chars[1] = 'x';
    const auto result = std::to_chars(chars.data() + 2, chars.data() + chars.size(), value, 16);
    if (result.ec != std::errc()) return {};
    const size_t written = static_cast<size_t>(result.ptr - (chars.data() + 2));
    const size_t padding = 16 - written;
    if (padding > 0)
    {
        std::move_backward(chars.data() + 2, chars.data() + 2 + written, chars.data() + 2 + written + padding);
        std::fill(chars.data() + 2, chars.data() + 2 + padding, '0');
    }
    return std::string(chars.data(), 18);
}

bool PackageReceiptSerializer::parseHash(const std::string& value, uint64_t& output)
{
    if (value.size() != 18 || value[0] != '0' || value[1] != 'x') return false;
    for (size_t i = 2; i < value.size(); ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    const auto result = std::from_chars(value.data() + 2, value.data() + value.size(), output, 16);
    return result.ec == std::errc() && result.ptr == value.data() + value.size();
}

bool PackageReceiptSerializer::canonicalize(PackageReceipt& receipt, std::vector<std::string>* errors_out)
{
    std::vector<std::string> errors;
    if (receipt.schema_version != SchemaVersion)
        errors.push_back(makeError("PKG-RECEIPT-SCHEMA", "unsupported schema version"));
    if (receipt.hash_algorithm != "fnv1a64")
        errors.push_back(makeError("PKG-RECEIPT-HASH", "unsupported hash algorithm"));
    if (receipt.package_name.empty())
        errors.push_back(makeError("PKG-RECEIPT-FIELD", "package name is empty"));
    if (receipt.project_name.empty())
        errors.push_back(makeError("PKG-RECEIPT-FIELD", "project name is empty"));
    if (receipt.default_level.empty())
        errors.push_back(makeError("PKG-RECEIPT-FIELD", "default level is empty"));
    else
    {
        std::string normalized;
        std::string path_error;
        if (!normalizePackageLogicalPath(receipt.default_level, normalized, &path_error))
            errors.push_back(makeError("PKG-RECEIPT-PATH", "default level " + path_error));
        else
            receipt.default_level = std::move(normalized);
    }
    if (receipt.game_module.empty())
        errors.push_back(makeError("PKG-RECEIPT-FIELD", "game module is empty"));
    else
    {
        std::string normalized;
        std::string path_error;
        if (!normalizePackageLogicalPath(receipt.game_module, normalized, &path_error))
            errors.push_back(makeError("PKG-RECEIPT-PATH", "game module " + path_error));
        else
            receipt.game_module = std::move(normalized);
    }

    PackageDependencyGraph graph;
    std::vector<PackageDependencyKey> seen_keys;
    for (const PackageDependency& dependency : receipt.dependencies)
    {
        const PackageDependencyKey key{dependency.kind, dependency.logical_path};
        if (std::find_if(seen_keys.begin(), seen_keys.end(), [&key](const PackageDependencyKey& seen) {
                return seen.kind == key.kind && seen.logical_path == key.logical_path;
            }) != seen_keys.end())
        {
            errors.push_back(makeError("PKG-RECEIPT-DUPLICATE", "duplicate dependency " + dependency.logical_path));
        }
        else
        {
            seen_keys.push_back(key);
        }
        graph.add(dependency);
    }
    const std::vector<std::string> graph_errors = graph.errors();
    errors.insert(errors.end(), graph_errors.begin(), graph_errors.end());
    receipt.dependencies = graph.dependencies();
    receipt.closure_hash_fnv1a64 = closureHash(receipt);

    sortErrors(errors);
    if (errors_out) *errors_out = errors;
    return errors.empty();
}

bool PackageReceiptSerializer::writeStaged(PackageReceipt receipt,
                                           const std::string& staged_package_root,
                                           std::string* error_out)
{
    if (error_out) error_out->clear();
    std::vector<std::string> errors;
    if (!canonicalize(receipt, &errors))
    {
        if (error_out) *error_out = errors.empty() ? "PKG-RECEIPT-INVALID" : errors.front();
        return false;
    }

    const fs::path root(staged_package_root);
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec)
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-STAGE", "staging root is unavailable: " + root.string());
        return false;
    }
    const fs::path final_path = root / Filename;
    const fs::path temporary_path = root / (std::string(Filename) + ".tmp." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            if (error_out) *error_out = makeError("PKG-RECEIPT-WRITE", "cannot create " + temporary_path.string());
            return false;
        }
        output << receiptJson(receipt, true).dump(4) << '\n';
        if (!output.good())
        {
            output.close();
            fs::remove(temporary_path, ec);
            if (error_out) *error_out = makeError("PKG-RECEIPT-WRITE", "cannot write " + temporary_path.string());
            return false;
        }
    }
    if (!atomicReplaceFile(temporary_path, final_path))
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-PUBLISH", "cannot publish " + final_path.string());
        return false;
    }
    return true;
}

bool PackageReceiptSerializer::readStaged(const std::string& package_root,
                                          PackageReceipt& receipt,
                                          std::string* error_out)
{
    if (error_out) error_out->clear();
    const fs::path path = fs::path(package_root) / Filename;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-READ", "cannot open " + path.string());
        return false;
    }

    json document;
    try { input >> document; }
    catch (const std::exception& exception)
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-PARSE", exception.what());
        return false;
    }
    static constexpr std::array<const char*, 10> fields = {
        "closure_hash_fnv1a64", "compile_config", "default_level", "dependencies", "engine_version",
        "game_module", "hash_algorithm", "package_name", "project_name", "schema_version"
    };
    if (!document.is_object())
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-PARSE", "receipt must be an object");
        return false;
    }
    for (const char* field : fields)
        if (!document.contains(field))
        {
            if (error_out) *error_out = makeError("PKG-RECEIPT-PARSE", std::string("receipt is missing ") + field);
            return false;
        }
    if (!document["schema_version"].is_number_unsigned() || !document["hash_algorithm"].is_string() ||
        !document["package_name"].is_string() || !document["project_name"].is_string() ||
        !document["engine_version"].is_string() || !document["default_level"].is_string() ||
        !document["game_module"].is_string() || !document["closure_hash_fnv1a64"].is_string() ||
        !document["dependencies"].is_array())
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-PARSE", "receipt contains a field with the wrong type");
        return false;
    }

    PackageReceipt parsed;
    parsed.schema_version = document["schema_version"].get<uint32_t>();
    parsed.hash_algorithm = document["hash_algorithm"].get<std::string>();
    parsed.package_name = document["package_name"].get<std::string>();
    parsed.project_name = document["project_name"].get<std::string>();
    parsed.engine_version = document["engine_version"].get<std::string>();
    parsed.default_level = document["default_level"].get<std::string>();
    parsed.game_module = document["game_module"].get<std::string>();
    std::string parse_error;
    if (!parseCompileConfig(document["compile_config"], parsed.compile_config, parse_error) ||
        !parseHash(document["closure_hash_fnv1a64"].get<std::string>(), parsed.closure_hash_fnv1a64))
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-PARSE", parse_error.empty() ? "invalid closure hash" : parse_error);
        return false;
    }
    for (const json& value : document["dependencies"])
    {
        PackageDependency dependency;
        if (!parseDependency(value, dependency, parse_error))
        {
            if (error_out) *error_out = makeError("PKG-RECEIPT-PARSE", parse_error);
            return false;
        }
        parsed.dependencies.push_back(std::move(dependency));
    }

    const uint64_t supplied_hash = parsed.closure_hash_fnv1a64;
    std::vector<std::string> errors;
    if (!canonicalize(parsed, &errors))
    {
        if (error_out) *error_out = errors.empty() ? "PKG-RECEIPT-INVALID" : errors.front();
        return false;
    }
    if (parsed.closure_hash_fnv1a64 != supplied_hash)
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-HASH", "closure hash mismatch");
        return false;
    }

    // canonicalize sorts only after proving the source receipt order. The on-disk
    // order itself is a determinism contract and must not be silently repaired.
    std::vector<PackageDependency> on_disk;
    for (const json& value : document["dependencies"])
    {
        PackageDependency dependency;
        parseDependency(value, dependency, parse_error);
        on_disk.push_back(std::move(dependency));
    }
    if (!std::is_sorted(on_disk.begin(), on_disk.end(), receiptDependencyLess))
    {
        if (error_out) *error_out = makeError("PKG-RECEIPT-ORDER", "dependencies are not in canonical order");
        return false;
    }
    for (size_t index = 1; index < on_disk.size(); ++index)
        if (sameReceiptDependency(on_disk[index - 1], on_disk[index]))
        {
            if (error_out) *error_out = makeError("PKG-RECEIPT-DUPLICATE", "duplicate dependency " + on_disk[index].logical_path);
            return false;
        }

    receipt = std::move(parsed);
    return true;
}

PackageReceiptVerification PackageReceiptSerializer::verifyStaged(const std::string& package_root)
{
    PackageReceiptVerification verification;
    PackageReceipt receipt;
    std::string error;
    if (!readStaged(package_root, receipt, &error))
    {
        verification.errors.push_back(error);
        return verification;
    }

    const fs::path root(package_root);
    for (const PackageDependency& dependency : receipt.dependencies)
    {
        const fs::path artifact = root / fs::path(dependency.logical_path);
        std::error_code ec;
        if (!fs::is_regular_file(artifact, ec) || ec)
        {
            verification.errors.push_back(makeError("PKG-VERIFY-MISSING", dependency.logical_path));
            continue;
        }
        const uint64_t size = fs::file_size(artifact, ec);
        if (ec || size != dependency.byte_size)
        {
            verification.errors.push_back(makeError("PKG-VERIFY-SIZE", dependency.logical_path));
            continue;
        }
        const uint64_t hash = Utils::hashFile(artifact.string());
        if (hash != dependency.cooked_hash_fnv1a64)
            verification.errors.push_back(makeError("PKG-VERIFY-HASH", dependency.logical_path));
    }
    sortErrors(verification.errors);
    verification.valid = verification.errors.empty();
    return verification;
}