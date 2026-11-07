#include <Assets/MeshData.h>

#include <limits>
#include <utility>

namespace cue
{
MeshData::MeshData(MeshDescriptor a_descriptor) noexcept : m_descriptor(std::move(a_descriptor))
{
}

Result<MeshData> MeshData::create(MeshDescriptor a_descriptor)
{
    // 同じ Index で全属性を参照するため、任意属性も頂点数を一致させる
    const auto count = a_descriptor.positions.size();
    if (a_descriptor.indices.valueless_by_exception() || count == 0 ||
        count > (std::numeric_limits<std::uint32_t>::max)() ||
        (!a_descriptor.normals.empty() && a_descriptor.normals.size() != count) ||
        (!a_descriptor.uv0.empty() && a_descriptor.uv0.size() != count))
    {
        return Result<MeshData>::failure({ErrorCategory::InvalidArgument, "MeshData.create.attributes"});
    }
    for (const auto &position : a_descriptor.positions)
    {
        if (!math::is_finite(position))
            return Result<MeshData>::failure({ErrorCategory::InvalidArgument, "MeshData.create.position"});
    }
    for (const auto &uv : a_descriptor.uv0)
    {
        if (!math::is_finite(uv))
            return Result<MeshData>::failure({ErrorCategory::InvalidArgument, "MeshData.create.uv"});
    }
    for (auto &normal : a_descriptor.normals)
    {
        auto normalized = math::normalize(normal);
        if (!normalized.has_value())
            return Result<MeshData>::failure({ErrorCategory::InvalidArgument, "MeshData.create.normal"});
        normal = normalized.take_value();
    }
    const bool areIndicesValid = std::visit(
        [count](const auto &a_indices)
        {
            if (a_indices.empty() || a_indices.size() % 3 != 0 ||
                a_indices.size() > (std::numeric_limits<std::uint32_t>::max)())
                return false;
            for (const auto index : a_indices)
                if (index >= count)
                    return false;
            return true;
        },
        a_descriptor.indices);
    if (!areIndicesValid)
        return Result<MeshData>::failure({ErrorCategory::InvalidArgument, "MeshData.create.indices"});
    return Result<MeshData>::success(MeshData(std::move(a_descriptor)));
}

std::span<const math::float3> MeshData::positions() const noexcept
{
    return m_descriptor.positions;
}
std::span<const math::float3> MeshData::normals() const noexcept
{
    return m_descriptor.normals;
}
std::span<const math::float2> MeshData::uv0() const noexcept
{
    return m_descriptor.uv0;
}
MeshIndexFormat MeshData::index_format() const noexcept
{
    return std::holds_alternative<std::vector<std::uint16_t>>(m_descriptor.indices) ? MeshIndexFormat::Uint16
                                                                                    : MeshIndexFormat::Uint32;
}
std::uint32_t MeshData::index_count() const noexcept
{
    return std::visit([](const auto &a_indices) { return static_cast<std::uint32_t>(a_indices.size()); },
                      m_descriptor.indices);
}
std::span<const std::uint16_t> MeshData::indices16() const noexcept
{
    const auto *indices = std::get_if<std::vector<std::uint16_t>>(&m_descriptor.indices);
    return indices ? std::span<const std::uint16_t>(*indices) : std::span<const std::uint16_t>{};
}
std::span<const std::uint32_t> MeshData::indices32() const noexcept
{
    const auto *indices = std::get_if<std::vector<std::uint32_t>>(&m_descriptor.indices);
    return indices ? std::span<const std::uint32_t>(*indices) : std::span<const std::uint32_t>{};
}
} // namespace cue
