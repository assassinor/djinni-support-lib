//
// Copyright 2014 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include "djinni_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace djinni {

struct Bool {
    using CppType = bool;
    using NapiType = napi_value;
    using Boxed = Bool;

    static CppType toCpp(napi_env env, NapiType value) {
        bool result = false;
        DJINNI_NAPI_CALL(env, napi_get_value_bool(env, value, &result));
        return result;
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_get_boolean(env, value, &result));
        return result;
    }
};

struct I32 {
    using CppType = int32_t;
    using NapiType = napi_value;
    using Boxed = I32;

    static CppType toCpp(napi_env env, NapiType value) {
        int32_t result = 0;
        DJINNI_NAPI_CALL(env, napi_get_value_int32(env, value, &result));
        return result;
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_int32(env, value, &result));
        return result;
    }
};

struct I8 {
    using CppType = int8_t;
    using NapiType = napi_value;
    using Boxed = I8;

    static CppType toCpp(napi_env env, NapiType value) {
        return static_cast<CppType>(I32::toCpp(env, value));
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        return I32::fromCpp(env, value);
    }
};

struct I16 {
    using CppType = int16_t;
    using NapiType = napi_value;
    using Boxed = I16;

    static CppType toCpp(napi_env env, NapiType value) {
        return static_cast<CppType>(I32::toCpp(env, value));
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        return I32::fromCpp(env, value);
    }
};

struct I64 {
    using CppType = int64_t;
    using NapiType = napi_value;
    using Boxed = I64;

    static CppType toCpp(napi_env env, NapiType value) {
        int64_t result = 0;
        bool lossless = false;
        DJINNI_NAPI_CALL(env, napi_get_value_bigint_int64(env, value, &result, &lossless));
        DJINNI_NAPI_ASSERT(env, lossless, "i64 value cannot be represented losslessly")
        return result;
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_bigint_int64(env, value, &result));
        return result;
    }
};

struct F64 {
    using CppType = double;
    using NapiType = napi_value;
    using Boxed = F64;

    static CppType toCpp(napi_env env, NapiType value) {
        double result = 0;
        DJINNI_NAPI_CALL(env, napi_get_value_double(env, value, &result));
        return result;
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_double(env, value, &result));
        return result;
    }
};

struct F32 {
    using CppType = float;
    using NapiType = napi_value;
    using Boxed = F32;

    static CppType toCpp(napi_env env, NapiType value) {
        return static_cast<CppType>(F64::toCpp(env, value));
    }

    static NapiType fromCpp(napi_env env, CppType value) {
        return F64::fromCpp(env, value);
    }
};

struct String {
    using CppType = std::string;
    using NapiType = napi_value;
    using Boxed = String;

    static CppType toCpp(napi_env env, NapiType value) {
        size_t length = 0;
        DJINNI_NAPI_CALL(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length));
        std::vector<char> buffer(length + 1, '\0');
        DJINNI_NAPI_CALL(env, napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &length));
        return std::string(buffer.data(), length);
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_string_utf8(env, value.c_str(), value.size(), &result));
        return result;
    }
};

struct WString {
    using CppType = std::wstring;
    using NapiType = napi_value;
    using Boxed = WString;

    static CppType toCpp(napi_env env, NapiType value) {
        auto text = String::toCpp(env, value);
        return CppType(text.begin(), text.end());
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        return String::fromCpp(env, std::string(value.begin(), value.end()));
    }
};

template <template <class...> class OptionalT, class T>
struct Optional {
    template <typename C> static OptionalT<typename C::CppType> opt_type(...);
    template <typename C> static typename C::CppOptType opt_type(typename C::CppOptType *);
    using CppType = decltype(opt_type<T>(nullptr));
    using NapiType = napi_value;
    using Boxed = Optional;

    static CppType toCpp(napi_env env, NapiType value) {
        if (isNullOrUndefined(env, value)) {
            return CppType{};
        }
        return T::Boxed::toCpp(env, value);
    }

    static NapiType fromCpp(napi_env env, const OptionalT<typename T::CppType> & value) {
        if (!value) {
            return undefined(env);
        }
        return T::Boxed::fromCpp(env, *value);
    }

    template <typename C = T>
    static NapiType fromCpp(napi_env env, const typename C::CppOptType & value) {
        return T::Boxed::fromCppOpt(env, value);
    }
};

struct Binary {
    using CppType = std::vector<uint8_t>;
    using NapiType = napi_value;
    using Boxed = Binary;

    static CppType toCpp(napi_env env, NapiType value) {
        void * data = nullptr;
        size_t length = 0;
        bool isTypedArray = false;
        DJINNI_NAPI_CALL(env, napi_is_typedarray(env, value, &isTypedArray));
        if (isTypedArray) {
            napi_typedarray_type type;
            napi_value arrayBuffer;
            size_t byteOffset = 0;
            DJINNI_NAPI_CALL(env, napi_get_typedarray_info(env, value, &type, &length, &data, &arrayBuffer, &byteOffset));
            (void)arrayBuffer;
            (void)byteOffset;
            DJINNI_NAPI_ASSERT(env, type == napi_uint8_array || type == napi_uint8_clamped_array, "binary value must be Uint8Array")
            auto * bytes = static_cast<uint8_t *>(data);
            return CppType(bytes, bytes + length);
        }

        DJINNI_NAPI_CALL(env, napi_get_arraybuffer_info(env, value, &data, &length));
        auto * bytes = static_cast<uint8_t *>(data);
        return CppType(bytes, bytes + length);
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        void * data = nullptr;
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_arraybuffer(env, value.size(), &data, &result));
        if (data != nullptr && !value.empty()) {
            std::memcpy(data, value.data(), value.size());
        }
        return result;
    }
};

struct Date {
    using CppType = std::chrono::system_clock::time_point;
    using NapiType = napi_value;
    using Boxed = Date;

    static CppType toCpp(napi_env env, NapiType value) {
        double millis = 0;
        DJINNI_NAPI_CALL(env, napi_get_date_value(env, value, &millis));
        return CppType(std::chrono::milliseconds(static_cast<int64_t>(millis)));
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_date(env, static_cast<double>(millis), &result));
        return result;
    }
};

template <class T>
struct List {
    using CppType = std::vector<typename T::CppType>;
    using NapiType = napi_value;
    using Boxed = List;

    static CppType toCpp(napi_env env, NapiType value) {
        uint32_t length = 0;
        DJINNI_NAPI_CALL(env, napi_get_array_length(env, value, &length));
        CppType result;
        result.reserve(length);
        for (uint32_t i = 0; i < length; ++i) {
            NapiHandleScope scope(env);
            napi_value element;
            DJINNI_NAPI_CALL(env, napi_get_element(env, value, i, &element));
            result.push_back(T::Boxed::toCpp(env, element));
        }
        return result;
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_create_array_with_length(env, value.size(), &result));
        uint32_t index = 0;
        for (const auto & element : value) {
            NapiHandleScope scope(env);
            DJINNI_NAPI_CALL(env, napi_set_element(env, result, index++, T::Boxed::fromCpp(env, element)));
        }
        return result;
    }
};

template <class T>
struct Set {
    using CppType = std::unordered_set<typename T::CppType>;
    using NapiType = napi_value;
    using Boxed = Set;

    static CppType toCpp(napi_env env, NapiType value) {
        napi_value global;
        DJINNI_NAPI_CALL(env, napi_get_global(env, &global));
        napi_value arrayConstructor = getProperty(env, global, "Array");
        napi_value from = getProperty(env, arrayConstructor, "from");
        napi_value array;
        DJINNI_NAPI_CALL(env, napi_call_function(env, arrayConstructor, from, 1, &value, &array));
        auto list = List<T>::toCpp(env, array);
        return CppType(list.begin(), list.end());
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        typename List<T>::CppType list(value.begin(), value.end());
        napi_value array = List<T>::fromCpp(env, list);
        napi_value global;
        DJINNI_NAPI_CALL(env, napi_get_global(env, &global));
        napi_value setConstructor = getProperty(env, global, "Set");
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_new_instance(env, setConstructor, 1, &array, &result));
        return result;
    }
};

template <class K, class V>
struct Map {
    using CppType = std::unordered_map<typename K::CppType, typename V::CppType>;
    using NapiType = napi_value;
    using Boxed = Map;

    static CppType toCpp(napi_env env, NapiType value) {
        napi_value global;
        DJINNI_NAPI_CALL(env, napi_get_global(env, &global));
        napi_value arrayConstructor = getProperty(env, global, "Array");
        napi_value from = getProperty(env, arrayConstructor, "from");
        napi_value entries;
        DJINNI_NAPI_CALL(env, napi_call_function(env, arrayConstructor, from, 1, &value, &entries));

        uint32_t length = 0;
        DJINNI_NAPI_CALL(env, napi_get_array_length(env, entries, &length));
        CppType result;
        for (uint32_t i = 0; i < length; ++i) {
            NapiHandleScope scope(env);
            napi_value entry;
            napi_value key;
            napi_value element;
            DJINNI_NAPI_CALL(env, napi_get_element(env, entries, i, &entry));
            DJINNI_NAPI_CALL(env, napi_get_element(env, entry, 0, &key));
            DJINNI_NAPI_CALL(env, napi_get_element(env, entry, 1, &element));
            result.emplace(K::Boxed::toCpp(env, key), V::Boxed::toCpp(env, element));
        }
        return result;
    }

    static NapiType fromCpp(napi_env env, const CppType & value) {
        napi_value entries;
        DJINNI_NAPI_CALL(env, napi_create_array_with_length(env, value.size(), &entries));
        uint32_t index = 0;
        for (const auto & pair : value) {
            NapiHandleScope scope(env);
            napi_value entry;
            DJINNI_NAPI_CALL(env, napi_create_array_with_length(env, 2, &entry));
            DJINNI_NAPI_CALL(env, napi_set_element(env, entry, 0, K::Boxed::fromCpp(env, pair.first)));
            DJINNI_NAPI_CALL(env, napi_set_element(env, entry, 1, V::Boxed::fromCpp(env, pair.second)));
            DJINNI_NAPI_CALL(env, napi_set_element(env, entries, index++, entry));
        }

        napi_value global;
        DJINNI_NAPI_CALL(env, napi_get_global(env, &global));
        napi_value mapConstructor = getProperty(env, global, "Map");
        napi_value result;
        DJINNI_NAPI_CALL(env, napi_new_instance(env, mapConstructor, 1, &entries, &result));
        return result;
    }
};

} // namespace djinni
