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

#include "djinni/djinni_common.hpp"
#include "djinni/proxy_cache_interface.hpp"

#include <node_api.h>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

#define DJINNI_NAPI_CALL(env, call) ::djinni::napiCall((env), (call), #call, __FILE__, __LINE__)
#define DJINNI_NAPI_ASSERT(env, check, message) \
    do { \
        if (!(check)) { \
            ::djinni::throwError((env), (message)); \
        } \
    } while (false);

namespace djinni {

DJINNI_NORETURN_DEFINITION void throwError(napi_env env, const char * message);
void napiCall(napi_env env, napi_status status, const char * call, const char * file, int line);
void setPendingFromCurrent(napi_env env) noexcept;
void napiInit(napi_env env);
void napiShutdown();

napi_value undefined(napi_env env);
bool isNullOrUndefined(napi_env env, napi_value value);
bool isNativeRef(napi_env env, napi_value value);

napi_value getProperty(napi_env env, napi_value object, const char * name);
void setProperty(napi_env env, napi_value object, const char * name, napi_value value);
napi_value callMethod(napi_env env, napi_value object, const char * name, size_t argc, napi_value * argv);
bool hasMethod(napi_env env, napi_value object, const char * name);
uint64_t objectIdFromEts(napi_env env, napi_value object);
void registerCppProxyFactory(napi_env env, const char * name, napi_value factory);
napi_value createCppProxy(napi_env env, const char * name, napi_value nativeRef);
napi_value nativeRefFromAddress(napi_env env, void * address);
void * nativeRefAddress(napi_env env, napi_value nativeRef);
napi_value nativeRefFromHandleOwner(napi_env env, void * owner);
void * handleOwnerFromNativeRef(napi_env env, napi_value nativeRef);
void releaseNativeRefHandleOwner(void * owner) noexcept;
void attachCppProxyFinalizer(napi_env env, napi_value object, void * data, napi_finalize finalize);
void ensureJsThreadDispatcher(napi_env env);
void runOnJsThreadAsync(napi_env env, const std::function<void(napi_env)> & task);
void runOnJsThreadSyncImpl(napi_env env, const std::function<void(napi_env)> & task);

template <class F>
auto runOnJsThreadSync(napi_env env, F && fn) -> decltype(fn(env)) {
    using R = decltype(fn(env));
    if constexpr (std::is_void<R>::value) {
        runOnJsThreadSyncImpl(env, [&](napi_env callbackEnv) {
            fn(callbackEnv);
        });
    } else {
        std::optional<R> result;
        runOnJsThreadSyncImpl(env, [&](napi_env callbackEnv) {
            result.emplace(fn(callbackEnv));
        });
        return std::move(*result);
    }
}

class NapiArgs final {
public:
    NapiArgs(napi_env env, napi_callback_info info, size_t expected);

    napi_value operator[](size_t index) const;
    napi_value thisArg() const { return thisArg_; }
    size_t size() const { return args_.size(); }

private:
    napi_env env_;
    std::vector<napi_value> args_;
    napi_value thisArg_ = nullptr;
};

class NapiHandleScope final {
public:
    explicit NapiHandleScope(napi_env env);
    NapiHandleScope(const NapiHandleScope &) = delete;
    NapiHandleScope & operator=(const NapiHandleScope &) = delete;
    ~NapiHandleScope();

private:
    napi_env env_;
    napi_handle_scope scope_ = nullptr;
};

class GlobalRef {
public:
    GlobalRef() = default;
    GlobalRef(napi_env env, napi_value value);
    GlobalRef(const GlobalRef &) = delete;
    GlobalRef & operator=(const GlobalRef &) = delete;
    GlobalRef(GlobalRef && other) noexcept;
    GlobalRef & operator=(GlobalRef && other) noexcept;
    ~GlobalRef();

    napi_value get(napi_env env) const;
    explicit operator bool() const { return ref_ != nullptr; }

private:
    void reset() noexcept;

    napi_env env_ = nullptr;
    napi_ref ref_ = nullptr;
};

class EtsProxyBase {
public:
    EtsProxyBase(napi_env env, napi_value object);
    virtual ~EtsProxyBase();

    napi_env env() const { return env_; }
    napi_value object(napi_env env) const { return object_->get(env); }
    std::shared_ptr<GlobalRef> objectRef() const { return object_; }

private:
    napi_env env_;
    std::shared_ptr<GlobalRef> object_;
};

class NapiObjectRef {
public:
    NapiObjectRef() = default;
    NapiObjectRef(napi_env env, napi_value object, uint64_t objectId);

    napi_env env() const { return env_; }
    uint64_t get() const { return objectId_; }
    uint64_t objectId() const { return objectId_; }
    napi_value object(napi_env env) const { return object_->get(env); }
    explicit operator bool() const { return objectId_ != 0; }

private:
    napi_env env_ = nullptr;
    uint64_t objectId_ = 0;
    std::shared_ptr<GlobalRef> object_;
};

class NapiLocalRef {
public:
    NapiLocalRef() = default;
    NapiLocalRef(napi_env env, napi_value value) : env_(env), value_(value) {}

    napi_env env() const { return env_; }
    napi_value get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

private:
    napi_env env_ = nullptr;
    napi_value value_ = nullptr;
};

class NapiWeakRef {
public:
    class State;

    NapiWeakRef() = default;
    explicit NapiWeakRef(const NapiLocalRef & object);
    ~NapiWeakRef();

    NapiLocalRef lock() const;
    bool expired() const;

private:
    std::shared_ptr<State> state_;
};

template <class T>
const T & get(const T & value) noexcept { return value; }

template <class T>
const T & release(const T & value) noexcept { return value; }

struct NapiProxyCacheTraits {
    using UnowningImplPointer = uint64_t;
    using OwningImplPointer = NapiObjectRef;
    using OwningProxyPointer = std::shared_ptr<void>;
    using WeakProxyPointer = std::weak_ptr<void>;
    using UnowningImplPointerHash = std::hash<uint64_t>;
    using UnowningImplPointerEqual = std::equal_to<uint64_t>;
};

extern template class ProxyCache<NapiProxyCacheTraits>;
using NapiProxyCache = ProxyCache<NapiProxyCacheTraits>;
template <typename T> using NapiProxyHandle = NapiProxyCache::Handle<NapiObjectRef, T>;
template <typename T> using EtsProxyHandle = NapiProxyHandle<T>;

struct NapiCppProxyCacheTraits {
    using UnowningImplPointer = void *;
    using OwningImplPointer = std::shared_ptr<void>;
    using OwningProxyPointer = NapiLocalRef;
    using WeakProxyPointer = NapiWeakRef;
    using UnowningImplPointerHash = std::hash<void *>;
    using UnowningImplPointerEqual = std::equal_to<void *>;
};

extern template class ProxyCache<NapiCppProxyCacheTraits>;
using NapiCppProxyCache = ProxyCache<NapiCppProxyCacheTraits>;
template <class T> using CppProxyHandle = NapiCppProxyCache::Handle<std::shared_ptr<T>>;

template <class T>
class CppProxyHandleOwner final {
public:
    explicit CppProxyHandleOwner(std::shared_ptr<T> value) : handle_(new CppProxyHandle<T>(std::move(value))) {}

    const std::shared_ptr<T> & get(napi_env env) const {
        DJINNI_NAPI_ASSERT(env, handle_ != nullptr, "trying to use a destroyed object")
        return handle_->get();
    }

    void destroy() {
        handle_.reset();
    }

private:
    std::unique_ptr<CppProxyHandle<T>> handle_;
};

template <class T>
void finalizeCppProxyHandle(napi_env, void * data, void *) {
    releaseNativeRefHandleOwner(data);
    delete static_cast<CppProxyHandleOwner<T> *>(data);
}

template <class T>
const std::shared_ptr<T> & objectFromHandleAddress(napi_env env, napi_value nativeRef) {
    auto * owner = static_cast<CppProxyHandleOwner<T> *>(nativeRefAddress(env, nativeRef));
    DJINNI_NAPI_ASSERT(env, owner != nullptr, "nativeRef is null")
    return owner->get(env);
}

template <class T>
std::shared_ptr<T> nativeRefToCpp(napi_env env, napi_value nativeRef) {
    return objectFromHandleAddress<T>(env, nativeRef);
}

template <class T>
void destroyCppProxyHandle(napi_env env, napi_value nativeRef) {
    auto * owner = static_cast<CppProxyHandleOwner<T> *>(nativeRefAddress(env, nativeRef));
    if (owner) {
        owner->destroy();
    }
}

napi_env currentCppProxyEnv();
const char * currentCppProxyName();

class NapiCppProxyScope {
public:
    NapiCppProxyScope(napi_env env, const char * name);
    ~NapiCppProxyScope();

private:
    napi_env previousEnv_;
    const char * previousName_;
};

template <class T>
std::pair<NapiLocalRef, void *> newCppProxy(const std::shared_ptr<void> & cppObj) {
    auto env = currentCppProxyEnv();
    auto typed = std::static_pointer_cast<T>(cppObj);
    auto owner = std::make_unique<CppProxyHandleOwner<T>>(typed);
    napi_value cppProxy = createCppProxy(env, currentCppProxyName(), nativeRefFromHandleOwner(env, owner.get()));
    attachCppProxyFinalizer(env, cppProxy, owner.get(), &finalizeCppProxyHandle<T>);
    owner.release();
    return { NapiLocalRef(env, cppProxy), typed.get() };
}

template <class T>
napi_value cppProxyFromCpp(napi_env env, const char * name, const std::shared_ptr<T> & value) {
    NapiCppProxyScope scope(env, name);
    auto erased = std::static_pointer_cast<void>(value);
    return NapiCppProxyCache::get(typeid(std::shared_ptr<T>), erased, &newCppProxy<T>).get();
}

template <class I, class Proxy>
std::shared_ptr<I> getEtsProxy(napi_env env, napi_value value);

template <class I, class Self>
class NapiInterface {
public:
    using CppType = std::shared_ptr<I>;

    static CppType _fromNapi(napi_env env, napi_value value) {
        if (isNullOrUndefined(env, value)) {
            return nullptr;
        }

        if constexpr (Self::HasCppProxy) {
            if (isNativeRef(env, value)) {
                return nativeRefToCpp<I>(env, value);
            }
            if (hasMethod(env, value, "_djinni_getNativeRef")) {
                auto nativeRef = callMethod(env, value, "_djinni_getNativeRef", 0, nullptr);
                if (isNativeRef(env, nativeRef)) {
                    return nativeRefToCpp<I>(env, nativeRef);
                }
            }
        }

        if constexpr (Self::HasEtsProxy) {
            return getEtsProxy<I, typename Self::EtsProxy>(env, value);
        } else {
            DJINNI_NAPI_ASSERT(env, false, "interface cannot be converted from ETS")
            return nullptr;
        }
    }

    static napi_value _toNapi(napi_env env, const CppType & value) {
        if (!value) {
            return undefined(env);
        }

        if constexpr (Self::HasEtsProxy) {
            if (auto proxy = std::dynamic_pointer_cast<typename Self::EtsProxy>(value)) {
                return proxy->object(env);
            }
        }

        if constexpr (Self::HasCppProxy) {
            return cppProxyFromCpp<I>(env, Self::cppProxyName(), value);
        } else {
            DJINNI_NAPI_ASSERT(env, false, "interface cannot be converted from C++")
            return undefined(env);
        }
    }
};

template <class I, class Proxy>
std::shared_ptr<I> getEtsProxy(napi_env env, napi_value value) {
    auto objectId = objectIdFromEts(env, value);
    NapiObjectRef object(env, value, objectId);
    auto proxy = NapiProxyCache::get(
        typeid(Proxy),
        object,
        [](const NapiObjectRef & impl) -> std::pair<std::shared_ptr<void>, uint64_t> {
            auto typed = std::make_shared<Proxy>(impl.env(), impl.object(impl.env()), impl.objectId());
            return { typed, impl.objectId() };
        }
    );
    return std::static_pointer_cast<I>(std::static_pointer_cast<Proxy>(proxy));
}

} // namespace djinni
