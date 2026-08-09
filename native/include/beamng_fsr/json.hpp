// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bfsr {

/// A deliberately small JSON value type.
///
/// The bridge runs inside another process's address space and is loaded before
/// the game finishes initialising, so pulling in a large third-party JSON
/// library is a liability (allocator assumptions, static init order, binary
/// size). This covers exactly what the config file and the IPC protocol need:
/// objects, arrays, strings, numbers, bools, null. No comments, no trailing
/// commas, no \u escapes beyond BMP-to-UTF-8.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    explicit Json(bool v) : type_(Type::Bool), bool_(v) {}
    explicit Json(double v) : type_(Type::Number), num_(v) {}
    explicit Json(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    explicit Json(unsigned v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    explicit Json(long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    explicit Json(unsigned long long v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    explicit Json(std::string v) : type_(Type::String), str_(std::move(v)) {}
    explicit Json(const char* v) : type_(Type::String), str_(v ? v : "") {}

    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json array() { Json j; j.type_ = Type::Array; return j; }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }

    // Lenient accessors: they never throw and never assert. A malformed or
    // partially-written config must degrade to defaults, never take the game down.
    [[nodiscard]] bool asBool(bool def = false) const noexcept;
    [[nodiscard]] double asNumber(double def = 0.0) const noexcept;
    [[nodiscard]] int asInt(int def = 0) const noexcept;
    [[nodiscard]] std::string asString(const std::string& def = {}) const;

    [[nodiscard]] const Json& operator[](const std::string& key) const;
    [[nodiscard]] const Json& at(size_t index) const;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool has(const std::string& key) const;

    void set(const std::string& key, Json value);
    void push(Json value);

    [[nodiscard]] const std::vector<std::string>& keyOrder() const noexcept { return keyOrder_; }
    [[nodiscard]] const std::vector<Json>& items() const noexcept { return arr_; }

    /// `indent < 0` emits compact output.
    [[nodiscard]] std::string dump(int indent = 2) const;

    /// Returns a Null Json on any parse error and fills `error` when non-null.
    static Json parse(const std::string& text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
    std::vector<std::string> keyOrder_; ///< Preserves author order so diffs stay readable.
};

} // namespace bfsr
