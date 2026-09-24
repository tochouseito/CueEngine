#pragma once

#include <Cue/Foundation/Error.h>

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace cue
{
/// @brief 成功ValueまたはErrorのどちらか一方を所有する
///
/// Move-onlyな結果を呼出側へ渡す。取得した非所有PointerはResultの破棄・Moveで失効する
template <typename T>
class Result final
{
    static_assert(std::is_object_v<T>);
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>);

public:
    /// @brief 不正な空状態を作らせない
    Result() = delete;
    /// @brief 所有Valueの暗黙共有を防ぐ
    Result(const Result&) = delete;
    /// @brief 所有Valueの暗黙共有を防ぐ
    Result& operator=(const Result&) = delete;
    /// @brief 所有Valueを移す
    Result(Result&&) = default;
    /// @brief 所有Valueを移す
    Result& operator=(Result&&) = default;
    /// @brief 保持するValueまたはErrorを解放する
    ~Result() = default;

    /// @brief 成功Valueの所有権をResultへ移す
    [[nodiscard]] static Result success(T a_value)
    {
        // Variant の成功側だけを構築し、Value の所有権を移す
        return Result(std::in_place_index<0>, std::move(a_value));
    }

    /// @brief Errorの所有権をResultへ移す
    [[nodiscard]] static Result failure(Error a_error)
    {
        // Variant の失敗側だけを構築して Value との同時保持を防ぐ
        return Result(std::in_place_index<1>, std::move(a_error));
    }

    /// @brief 成功Valueを保持するか返す
    [[nodiscard]] bool has_value() const noexcept
    {
        return m_storage.index() == 0;
    }

    /// @brief 成功ValueをResultからMoveして呼出側へ渡す
    /// @pre has_value()がtrueであること。失敗状態で呼ぶとstd::bad_variant_accessを送出する
    /// @post Resultは成功状態のままだが、内部ValueはMove後の状態になる
    [[nodiscard]] T take_value() &
    {
        // 成功側以外では std::get が契約違反を例外として通知する
        return std::move(std::get<0>(m_storage));
    }

    /// @brief 成功Valueへの非所有Pointerを返し、失敗時はnullptrを返す
    [[nodiscard]] T* try_value() & noexcept
    {
        return std::get_if<0>(&m_storage);
    }

    /// @brief 成功Valueへの非所有Pointerを返し、失敗時はnullptrを返す
    [[nodiscard]] const T* try_value() const& noexcept
    {
        return std::get_if<0>(&m_storage);
    }

    /// @brief 一時Resultから失効するPointerを取得させない
    T* try_value() && = delete;

    /// @brief 一時Resultから失効するPointerを取得させない
    const T* try_value() const&& = delete;

    /// @brief Errorへの非所有Pointerを返し、成功時はnullptrを返す
    [[nodiscard]] Error* try_error() & noexcept
    {
        return std::get_if<1>(&m_storage);
    }

    /// @brief Errorへの非所有Pointerを返し、成功時はnullptrを返す
    [[nodiscard]] const Error* try_error() const& noexcept
    {
        return std::get_if<1>(&m_storage);
    }

    /// @brief 一時Resultから失効するPointerを取得させない
    Error* try_error() && = delete;

    /// @brief 一時Resultから失効するPointerを取得させない
    const Error* try_error() const&& = delete;

private:
    /// @brief 指定した側の値でResultを構築する
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> a_index, Value&& a_value)
        : m_storage(a_index, std::forward<Value>(a_value))
    {
    }

    std::variant<T, Error> m_storage;
};

/// @brief Valueを持たない操作の成功またはErrorを所有する
template <>
class Result<void> final
{
public:
    /// @brief 不正な空状態を作らせない
    Result() = delete;
    /// @brief Errorの暗黙共有を防ぐ
    Result(const Result&) = delete;
    /// @brief Errorの暗黙共有を防ぐ
    Result& operator=(const Result&) = delete;
    /// @brief Errorを移す
    Result(Result&&) = default;
    /// @brief Errorを移す
    Result& operator=(Result&&) = default;
    /// @brief 保持するErrorを解放する
    ~Result() = default;

    /// @brief 成功状態を作る
    [[nodiscard]] static Result success() noexcept
    {
        // Error がない状態を void 操作の成功として表す
        return Result(SuccessTag{});
    }

    /// @brief Errorの所有権をResultへ移す
    [[nodiscard]] static Result failure(Error a_error)
    {
        return Result(std::move(a_error));
    }

    /// @brief 成功状態か返す
    [[nodiscard]] bool has_value() const noexcept
    {
        return !m_error.has_value();
    }

    /// @brief Errorへの非所有Pointerを返し、成功時はnullptrを返す
    [[nodiscard]] Error* try_error() & noexcept
    {
        return m_error ? &*m_error : nullptr;
    }

    /// @brief Errorへの非所有Pointerを返し、成功時はnullptrを返す
    [[nodiscard]] const Error* try_error() const& noexcept
    {
        return m_error ? &*m_error : nullptr;
    }

    /// @brief 一時Resultから失効するPointerを取得させない
    Error* try_error() && = delete;

    /// @brief 一時Resultから失効するPointerを取得させない
    const Error* try_error() const&& = delete;

private:
    struct SuccessTag
    {
    };

    /// @brief Errorを持たない成功状態を構築する
    explicit Result(SuccessTag) noexcept
    {
    }

    /// @brief Errorを所有する失敗状態を構築する
    explicit Result(Error a_error)
        : m_error(std::move(a_error))
    {
    }

    std::optional<Error> m_error;
};
} // namespace cue
