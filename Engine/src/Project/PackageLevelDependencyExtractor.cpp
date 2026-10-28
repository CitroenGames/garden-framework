#include "PackageLevelDependencyExtractor.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <utility>

using json = nlohmann::json;

namespace
{
std::string fieldError(const std::string& level, const std::string& field,
                       const char* expected)
{
    return "PKG-LEVEL-FIELD: " + level + " " + field + " must be " + expected;
}

bool readRequiredPath(const json& object, const char* field,
                      const std::string& level, const std::string& location,
                      std::string& value, std::string& error)
{
    const auto found = object.find(field);
    if (found == object.end())
        return false;
    if (!found->is_string() || found->get_ref<const std::string&>().empty())
    {
        error = fieldError(level, location + "." + field, "a non-empty string");
        return false;
    }
    value = found->get<std::string>();
    return true;
}

bool isCookedCollisionPath(const std::string& path)
{
    constexpr std::string_view suffix = ".gardenphys";
    if (path.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), path.rbegin(),
        [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        });
}

bool isCurrentCanimPath(const std::string& path)
{
    constexpr std::string_view suffix = ".canim";
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void appendChild(std::vector<PackageDependencyRequest>& children,
                 std::string logical_path, PackageDependencyKind kind,
                 const std::string& owner)
{
    children.push_back({std::move(logical_path), kind, owner,
                        std::string(PackageLevelDependencyExtractor::SourceId)});
}
}

bool PackageLevelDependencyExtractor::extract(
    const std::string& level_logical_path, const std::string& staged_file_path,
    std::vector<PackageDependencyRequest>& children, std::string& error)
{
    children.clear();
    error.clear();

    if (!level_logical_path.ends_with(".level.json"))
    {
        error = "PKG-LEVEL-CONTRACT: " + level_logical_path +
                " must use the current .level.json source contract";
        return false;
    }

    std::ifstream input(staged_file_path);
    if (!input.is_open())
    {
        error = "PKG-LEVEL-READ: " + level_logical_path;
        return false;
    }

    json document;
    try
    {
        input >> document;
    }
    catch (const std::exception& exception)
    {
        error = "PKG-LEVEL-PARSE: " + level_logical_path + ": " + exception.what();
        return false;
    }

    if (!document.is_object())
    {
        error = fieldError(level_logical_path, "$", "an object");
        return false;
    }
    const auto entities = document.find("entities");
    if (entities == document.end() || !entities->is_array())
    {
        error = fieldError(level_logical_path, "entities", "an array");
        return false;
    }

    for (size_t index = 0; index < entities->size(); ++index)
    {
        const json& entity = (*entities)[index];
        const std::string entity_location = "entities[" + std::to_string(index) + "]";
        if (!entity.is_object())
        {
            error = fieldError(level_logical_path, entity_location, "an object");
            return false;
        }

        const auto mesh = entity.find("mesh");
        if (mesh != entity.end())
        {
            const std::string mesh_location = entity_location + ".mesh";
            if (!mesh->is_object())
            {
                error = fieldError(level_logical_path, mesh_location, "an object");
                return false;
            }

            std::string mesh_path;
            if (mesh->contains("path"))
            {
                if (!readRequiredPath(*mesh, "path", level_logical_path, mesh_location,
                                      mesh_path, error))
                    return false;
                appendChild(children, std::move(mesh_path), PackageDependencyKind::Mesh,
                            level_logical_path);
            }

            const auto textures = mesh->find("textures");
            if (textures != mesh->end())
            {
                if (!textures->is_array())
                {
                    error = fieldError(level_logical_path, mesh_location + ".textures", "an array");
                    return false;
                }
                for (size_t texture_index = 0; texture_index < textures->size(); ++texture_index)
                {
                    const json& texture = (*textures)[texture_index];
                    if (!texture.is_string() || texture.get_ref<const std::string&>().empty())
                    {
                        error = fieldError(level_logical_path,
                                           mesh_location + ".textures[" + std::to_string(texture_index) + "]",
                                           "a non-empty string");
                        return false;
                    }
                    appendChild(children, texture.get<std::string>(),
                                PackageDependencyKind::Texture, level_logical_path);
                }
            }
        }

        const auto collider = entity.find("collider");
        if (collider != entity.end())
        {
            const std::string collider_location = entity_location + ".collider";
            if (!collider->is_object())
            {
                error = fieldError(level_logical_path, collider_location, "an object");
                return false;
            }
            if (collider->contains("mesh_path"))
            {
                std::string collider_path;
                if (!readRequiredPath(*collider, "mesh_path", level_logical_path,
                                      collider_location, collider_path, error))
                    return false;
                appendChild(children, collider_path,
                            isCookedCollisionPath(collider_path)
                                ? PackageDependencyKind::Collision
                                : PackageDependencyKind::Mesh,
                            level_logical_path);
            }
        }

        const auto components = entity.find("components");
        if (components != entity.end())
        {
            const std::string components_location = entity_location + ".components";
            if (!components->is_object())
            {
                error = fieldError(level_logical_path, components_location, "an object");
                return false;
            }
            if (components->contains("AnimationComponent"))
            {
                error = "PKG-ANIMATION-RETIRED: " + level_logical_path + " " +
                        components_location + ".AnimationComponent is retired; use SkeletonComponent";
                return false;
            }
            const auto skeleton = components->find("SkeletonComponent");
            if (skeleton != components->end())
            {
                const std::string skeleton_location = components_location + ".SkeletonComponent";
                if (!skeleton->is_object())
                {
                    error = fieldError(level_logical_path, skeleton_location, "an object");
                    return false;
                }
                for (const char* retired : {"skeleton_path", "clip_names", "resources"})
                {
                    if (skeleton->contains(retired))
                    {
                        error = "PKG-ANIMATION-RETIRED: " + level_logical_path + " " +
                                skeleton_location + "." + retired + " is retired";
                        return false;
                    }
                }
                if (skeleton->contains("animation_resource_path"))
                {
                    std::string animation_path;
                    if (!readRequiredPath(*skeleton, "animation_resource_path", level_logical_path,
                                          skeleton_location, animation_path, error))
                        return false;
                    if (!isCurrentCanimPath(animation_path))
                    {
                        error = "PKG-ANIMATION-RETIRED: " + level_logical_path + " " +
                                skeleton_location + ".animation_resource_path must reference current .canim data";
                        return false;
                    }
                    appendChild(children, animation_path, PackageDependencyKind::AnimationClip,
                                level_logical_path);
                }
            }
        }
    }
    return true;
}
