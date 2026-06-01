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

#include "djinni_support.hpp"
#include "djinni/proxy_cache_impl.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <unordered_map>

namespace djinni {

namespace {

constexpr const char * kObjectIdProperty = "__djinni_napi_object_id";

void cleanupHook(void *);

struct JsThreadJob {
    std::function<void(napi_env)> task;
    std::mutex mutex;
    std::condition_variable cv;
    std::exception_ptr exception;
    bool async = false;
    bool done = false;
};

class JsThreadDispatcher {
public:
    bool ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tsfn_ != nullptr;
    }

    bool isJsThread() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tsfn_ != nullptr && std::this_thread::get_id() == jsThreadId_;
    }

    bool accepts(napi_env env) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return env_ == env && tsfn_ != nullptr;
    }

    void ensure(napi_env env) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tsfn_ != nullptr) {
            if (!cleanupHookRegistered_) {
                DJINNI_NAPI_CALL(env, napi_add_env_cleanup_hook(env, cleanupHook, nullptr));
                cleanupHookRegistered_ = true;
            }
            return;
        }

        env_ = env;
        jsThreadId_ = std::this_thread::get_id();

        napi_value name;
        DJINNI_NAPI_CALL(env, napi_create_string_utf8(env, "djinni_napi_dispatcher", NAPI_AUTO_LENGTH, &name));
        napi_value callback;
        DJINNI_NAPI_CALL(env, napi_create_function(env, "djinni_napi_dispatcher", NAPI_AUTO_LENGTH,
            [](napi_env env, napi_callback_info) -> napi_value {
                return undefined(env);
            },
            nullptr,
            &callback
        ));
        DJINNI_NAPI_CALL(env, napi_create_threadsafe_function(
            env,
            callback,
            nullptr,
            name,
            0,
            1,
            nullptr,
            nullptr,
            nullptr,
            [](napi_env env, napi_value, void *, void * data) {
                auto * job = static_cast<JsThreadJob *>(data);
                try {
                    job->task(env);
                } catch (...) {
                    job->exception = std::current_exception();
                }
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->done = true;
                }
                job->cv.notify_one();
                if (job->async) {
                    delete job;
                }
            },
            &tsfn_
        ));
        DJINNI_NAPI_CALL(env, napi_add_env_cleanup_hook(env, cleanupHook, nullptr));
        cleanupHookRegistered_ = true;
    }

    void shutdown(bool removeCleanupHook) noexcept {
        napi_env env = nullptr;
        napi_threadsafe_function tsfn = nullptr;
        bool cleanupHookRegistered = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            env = env_;
            tsfn = tsfn_;
            cleanupHookRegistered = cleanupHookRegistered_;
            env_ = nullptr;
            tsfn_ = nullptr;
            cleanupHookRegistered_ = false;
            jsThreadId_ = std::thread::id();
        }

        if (env != nullptr && cleanupHookRegistered && removeCleanupHook) {
            napi_remove_env_cleanup_hook(env, cleanupHook, nullptr);
        }
        if (tsfn != nullptr) {
            napi_release_threadsafe_function(tsfn, napi_tsfn_abort);
        }
    }

    void runAsync(napi_env env, const std::function<void(napi_env)> & task) {
        ensure(env);
        if (std::this_thread::get_id() == jsThreadId_) {
            task(env_);
            return;
        }

        auto * job = new JsThreadJob{task};
        job->async = true;
        napi_status status = napi_call_threadsafe_function(tsfn_, job, napi_tsfn_blocking);
        if (status != napi_ok) {
            delete job;
            DJINNI_NAPI_CALL(env_, status);
        }
    }

    void runAsyncNoexcept(napi_env env, const std::function<void(napi_env)> & task) noexcept {
        try {
            runAsync(env, task);
        } catch (...) {
        }
    }

    void runSync(napi_env env, const std::function<void(napi_env)> & task) {
        ensure(env);
        if (std::this_thread::get_id() == jsThreadId_) {
            task(env_);
            return;
        }

        JsThreadJob job{task};
        DJINNI_NAPI_CALL(env_, napi_call_threadsafe_function(tsfn_, &job, napi_tsfn_blocking));
        std::unique_lock<std::mutex> lock(job.mutex);
        job.cv.wait(lock, [&job] { return job.done; });
        if (job.exception) {
            std::rethrow_exception(job.exception);
        }
    }

private:
    mutable std::mutex mutex_;
    napi_env env_ = nullptr;
    std::thread::id jsThreadId_;
    napi_threadsafe_function tsfn_ = nullptr;
    bool cleanupHookRegistered_ = false;
};

JsThreadDispatcher & jsThreadDispatcher() {
    static JsThreadDispatcher dispatcher;
    return dispatcher;
}

std::mutex & cppProxyFactoryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, GlobalRef> & cppProxyFactories() {
    static std::unordered_map<std::string, GlobalRef> factories;
    return factories;
}

std::mutex & cppProxyHandleRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint64_t, void *> & cppProxyHandleRegistry() {
    static std::unordered_map<uint64_t, void *> handles;
    return handles;
}

std::unordered_map<void *, uint64_t> & cppProxyHandleReverseRegistry() {
    static std::unordered_map<void *, uint64_t> handles;
    return handles;
}

uint64_t & nextCppProxyHandleId() {
    static uint64_t next = 1;
    return next;
}

uint64_t nextObjectId() {
    static std::atomic<uint64_t> next { 1 };
    auto value = next.fetch_add(1, std::memory_order_relaxed);
    return value == 0 ? next.fetch_add(1, std::memory_order_relaxed) : value;
}

thread_local napi_env currentCppProxyEnvValue = nullptr;
thread_local const char * currentCppProxyNameValue = nullptr;

void deleteReferenceOnJsThread(napi_env env, napi_ref ref) noexcept {
    if (env == nullptr || ref == nullptr) {
        return;
    }

    auto & dispatcher = jsThreadDispatcher();
    if (!dispatcher.accepts(env)) {
        return;
    }
    if (dispatcher.ready() && !dispatcher.isJsThread()) {
        dispatcher.runAsyncNoexcept(env, [ref](napi_env callbackEnv) {
            napi_delete_reference(callbackEnv, ref);
        });
        return;
    }

    napi_delete_reference(env, ref);
}

void clearCppProxyFactories() noexcept {
    try {
        std::lock_guard<std::mutex> lock(cppProxyFactoryMutex());
        cppProxyFactories().clear();
    } catch (...) {
    }
}

void clearCppProxyHandleRegistry() noexcept {
    try {
        std::lock_guard<std::mutex> lock(cppProxyHandleRegistryMutex());
        cppProxyHandleRegistry().clear();
        cppProxyHandleReverseRegistry().clear();
    } catch (...) {
    }
}

void napiShutdownImpl(bool removeCleanupHook) noexcept {
    clearCppProxyFactories();
    clearCppProxyHandleRegistry();
    jsThreadDispatcher().shutdown(removeCleanupHook);
}

void cleanupHook(void *) {
    napiShutdownImpl(false);
}

} // namespace

template class ProxyCache<NapiProxyCacheTraits>;
template class ProxyCache<NapiCppProxyCacheTraits>;

void throwError(napi_env env, const char * message) {
    napi_throw_error(env, nullptr, message);
    throw std::runtime_error(message);
}

void napiCall(napi_env env, napi_status status, const char * call, const char * file, int line) {
    if (status == napi_ok) {
        return;
    }

    char message[512];
    std::snprintf(message, sizeof(message), "NAPI call failed at %s:%d: %s", file, line, call);
    throwError(env, message);
}

void setPendingFromCurrent(napi_env env) noexcept {
    try {
        throw;
    } catch (const std::exception & e) {
        napi_throw_error(env, nullptr, e.what());
    } catch (...) {
        napi_throw_error(env, nullptr, "unknown C++ exception");
    }
}

void napiInit(napi_env env) {
    ensureJsThreadDispatcher(env);
}

void napiShutdown() {
    napiShutdownImpl(true);
}

napi_value undefined(napi_env env) {
    napi_value value;
    DJINNI_NAPI_CALL(env, napi_get_undefined(env, &value));
    return value;
}

bool isNullOrUndefined(napi_env env, napi_value value) {
    if (value == nullptr) {
        return true;
    }
    napi_valuetype type;
    DJINNI_NAPI_CALL(env, napi_typeof(env, value, &type));
    return type == napi_null || type == napi_undefined;
}

NapiArgs::NapiArgs(napi_env env, napi_callback_info info, size_t expected) : env_(env) {
    args_.resize(expected);
    size_t argc = expected;
    DJINNI_NAPI_CALL(env_, napi_get_cb_info(env_, info, &argc, args_.data(), &thisArg_, nullptr));
    DJINNI_NAPI_ASSERT(env_, argc >= expected, "not enough arguments")
    args_.resize(argc);
}

napi_value NapiArgs::operator[](size_t index) const {
    DJINNI_NAPI_ASSERT(env_, index < args_.size(), "argument index out of range")
    return args_[index];
}

NapiHandleScope::NapiHandleScope(napi_env env) : env_(env) {
    DJINNI_NAPI_CALL(env_, napi_open_handle_scope(env_, &scope_));
}

NapiHandleScope::~NapiHandleScope() {
    if (env_ != nullptr && scope_ != nullptr) {
        napi_close_handle_scope(env_, scope_);
    }
}

bool isNativeRef(napi_env env, napi_value value) {
    if (isNullOrUndefined(env, value)) {
        return false;
    }
    double handle = 0;
    return napi_get_value_double(env, value, &handle) == napi_ok && handle > 0;
}

napi_value getProperty(napi_env env, napi_value object, const char * name) {
    napi_value key;
    napi_value value;
    DJINNI_NAPI_CALL(env, napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &key));
    DJINNI_NAPI_CALL(env, napi_get_property(env, object, key, &value));
    return value;
}

void setProperty(napi_env env, napi_value object, const char * name, napi_value value) {
    napi_value key;
    DJINNI_NAPI_CALL(env, napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &key));
    DJINNI_NAPI_CALL(env, napi_set_property(env, object, key, value));
}

napi_value callMethod(napi_env env, napi_value object, const char * name, size_t argc, napi_value * argv) {
    napi_value method = getProperty(env, object, name);
    napi_value result;
    DJINNI_NAPI_CALL(env, napi_call_function(env, object, method, argc, argv, &result));
    return result;
}

bool hasMethod(napi_env env, napi_value object, const char * name) {
    if (isNullOrUndefined(env, object)) {
        return false;
    }

    napi_valuetype objectType;
    DJINNI_NAPI_CALL(env, napi_typeof(env, object, &objectType));
    if (objectType != napi_object && objectType != napi_function) {
        return false;
    }

    napi_value method = getProperty(env, object, name);
    napi_valuetype methodType;
    DJINNI_NAPI_CALL(env, napi_typeof(env, method, &methodType));
    return methodType == napi_function;
}

uint64_t objectIdFromEts(napi_env env, napi_value object) {
    DJINNI_NAPI_ASSERT(env, !isNullOrUndefined(env, object), "ETS object is null")

    napi_value existing = nullptr;
    napi_status getStatus = napi_get_named_property(env, object, kObjectIdProperty, &existing);
    if (getStatus == napi_ok && !isNullOrUndefined(env, existing)) {
        uint64_t objectId = 0;
        bool lossless = false;
        if (napi_get_value_bigint_uint64(env, existing, &objectId, &lossless) == napi_ok && lossless && objectId > 0) {
            return objectId;
        }
    }

    auto objectId = nextObjectId();
    napi_value property;
    DJINNI_NAPI_CALL(env, napi_create_bigint_uint64(env, objectId, &property));
    napi_property_descriptor descriptor {
        kObjectIdProperty,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        property,
        napi_default,
        nullptr,
    };
    DJINNI_NAPI_CALL(env, napi_define_properties(env, object, 1, &descriptor));
    return objectId;
}

void registerCppProxyFactory(napi_env env, const char * name, napi_value factory) {
    std::lock_guard<std::mutex> lock(cppProxyFactoryMutex());
    cppProxyFactories().insert_or_assign(name, GlobalRef(env, factory));
}

napi_value createCppProxy(napi_env env, const char * name, napi_value nativeRef) {
    GlobalRef factory;
    {
        std::lock_guard<std::mutex> lock(cppProxyFactoryMutex());
        auto it = cppProxyFactories().find(name);
        DJINNI_NAPI_ASSERT(env, it != cppProxyFactories().end(), "CppProxy factory is not registered")
        factory = GlobalRef(env, it->second.get(env));
    }

    napi_value global;
    DJINNI_NAPI_CALL(env, napi_get_global(env, &global));
    napi_value args[] = { nativeRef };
    napi_value result;
    DJINNI_NAPI_CALL(env, napi_call_function(env, global, factory.get(env), 1, args, &result));
    return result;
}

napi_value nativeRefFromAddress(napi_env env, void * address) {
    return nativeRefFromHandleOwner(env, address);
}

void * nativeRefAddress(napi_env env, napi_value nativeRef) {
    return handleOwnerFromNativeRef(env, nativeRef);
}

napi_value nativeRefFromHandleOwner(napi_env env, void * owner) {
    DJINNI_NAPI_ASSERT(env, owner != nullptr, "nativeRef owner is null")

    uint64_t handle = 0;
    {
        std::lock_guard<std::mutex> lock(cppProxyHandleRegistryMutex());
        auto & reverseRegistry = cppProxyHandleReverseRegistry();
        auto reverseIt = reverseRegistry.find(owner);
        if (reverseIt != reverseRegistry.end()) {
            handle = reverseIt->second;
        } else {
            handle = nextCppProxyHandleId()++;
            if (handle == 0) {
                handle = nextCppProxyHandleId()++;
            }
            cppProxyHandleRegistry().emplace(handle, owner);
            reverseRegistry.emplace(owner, handle);
        }
    }

    napi_value value;
    DJINNI_NAPI_CALL(env, napi_create_double(env, static_cast<double>(handle), &value));
    return value;
}

void * handleOwnerFromNativeRef(napi_env env, napi_value nativeRef) {
    double nativeRefId = 0;
    DJINNI_NAPI_CALL(env, napi_get_value_double(env, nativeRef, &nativeRefId));
    DJINNI_NAPI_ASSERT(env, nativeRefId > 0, "nativeRef handle is invalid")
    auto handle = static_cast<uint64_t>(nativeRefId);
    DJINNI_NAPI_ASSERT(env, static_cast<double>(handle) == nativeRefId, "nativeRef handle is not an integer")

    std::lock_guard<std::mutex> lock(cppProxyHandleRegistryMutex());
    auto it = cppProxyHandleRegistry().find(handle);
    DJINNI_NAPI_ASSERT(env, it != cppProxyHandleRegistry().end(), "nativeRef handle is stale")
    return it->second;
}

void releaseNativeRefHandleOwner(void * owner) noexcept {
    if (owner == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(cppProxyHandleRegistryMutex());
    auto & reverseRegistry = cppProxyHandleReverseRegistry();
    auto reverseIt = reverseRegistry.find(owner);
    if (reverseIt == reverseRegistry.end()) {
        return;
    }
    cppProxyHandleRegistry().erase(reverseIt->second);
    reverseRegistry.erase(reverseIt);
}

void attachCppProxyFinalizer(napi_env env, napi_value object, void * data, napi_finalize finalize) {
    DJINNI_NAPI_CALL(env, napi_add_finalizer(env, object, data, finalize, nullptr, nullptr));
}

napi_env currentCppProxyEnv() {
    return currentCppProxyEnvValue;
}

const char * currentCppProxyName() {
    return currentCppProxyNameValue;
}

NapiCppProxyScope::NapiCppProxyScope(napi_env env, const char * name)
    : previousEnv_(currentCppProxyEnvValue), previousName_(currentCppProxyNameValue) {
    currentCppProxyEnvValue = env;
    currentCppProxyNameValue = name;
}

NapiCppProxyScope::~NapiCppProxyScope() {
    currentCppProxyEnvValue = previousEnv_;
    currentCppProxyNameValue = previousName_;
}

void ensureJsThreadDispatcher(napi_env env) {
    jsThreadDispatcher().ensure(env);
}

void runOnJsThreadAsync(napi_env env, const std::function<void(napi_env)> & task) {
    jsThreadDispatcher().runAsync(env, task);
}

void runOnJsThreadSyncImpl(napi_env env, const std::function<void(napi_env)> & task) {
    jsThreadDispatcher().runSync(env, task);
}

GlobalRef::GlobalRef(napi_env env, napi_value value) : env_(env) {
    DJINNI_NAPI_CALL(env_, napi_create_reference(env_, value, 1, &ref_));
}

GlobalRef::GlobalRef(GlobalRef && other) noexcept : env_(other.env_), ref_(other.ref_) {
    other.env_ = nullptr;
    other.ref_ = nullptr;
}

GlobalRef & GlobalRef::operator=(GlobalRef && other) noexcept {
    if (this != &other) {
        reset();
        env_ = other.env_;
        ref_ = other.ref_;
        other.env_ = nullptr;
        other.ref_ = nullptr;
    }
    return *this;
}

GlobalRef::~GlobalRef() {
    reset();
}

napi_value GlobalRef::get(napi_env env) const {
    napi_value value = nullptr;
    DJINNI_NAPI_ASSERT(env, ref_ != nullptr, "global reference is empty")
    DJINNI_NAPI_CALL(env, napi_get_reference_value(env, ref_, &value));
    return value;
}

void GlobalRef::reset() noexcept {
    if (env_ != nullptr && ref_ != nullptr) {
        deleteReferenceOnJsThread(env_, ref_);
    }
    env_ = nullptr;
    ref_ = nullptr;
}

EtsProxyBase::EtsProxyBase(napi_env env, napi_value object)
    : env_(env), object_(std::make_shared<GlobalRef>(env, object)) {
    ensureJsThreadDispatcher(env);
}
EtsProxyBase::~EtsProxyBase() = default;

NapiObjectRef::NapiObjectRef(napi_env env, napi_value object, uint64_t objectId)
    : env_(env), objectId_(objectId), object_(std::make_shared<GlobalRef>(env, object)) {
    ensureJsThreadDispatcher(env);
}

class NapiWeakRef::State {
public:
    State(napi_env env, napi_value object) : env_(env) {
        DJINNI_NAPI_CALL(env_, napi_create_reference(env_, object, 0, &ref_));
    }

    ~State() {
        if (env_ != nullptr && ref_ != nullptr) {
            deleteReferenceOnJsThread(env_, ref_);
        }
    }

    NapiLocalRef lock() const {
        napi_value object = nullptr;
        if (env_ == nullptr || ref_ == nullptr || napi_get_reference_value(env_, ref_, &object) != napi_ok || object == nullptr) {
            return {};
        }
        return NapiLocalRef(env_, object);
    }

private:
    napi_env env_ = nullptr;
    napi_ref ref_ = nullptr;
};

NapiWeakRef::NapiWeakRef(const NapiLocalRef & object)
    : state_(object ? std::make_shared<State>(object.env(), object.get()) : nullptr) {}

NapiWeakRef::~NapiWeakRef() = default;

NapiLocalRef NapiWeakRef::lock() const {
    return state_ ? state_->lock() : NapiLocalRef();
}

bool NapiWeakRef::expired() const {
    return !lock();
}

} // namespace djinni
