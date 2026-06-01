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

#include "djinni_napi_main.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace djinni {

namespace {

std::vector<std::unique_ptr<napi_module>> & registeredModules() {
    static std::vector<std::unique_ptr<napi_module>> modules;
    return modules;
}

std::mutex & registeredModulesMutex() {
    static std::mutex mutex;
    return mutex;
}

}

void registerNapiModule(const char * moduleName, napi_addon_register_func registerFunc) {
    auto module = std::make_unique<napi_module>();
    module->nm_version = 1;
    module->nm_flags = 0;
    module->nm_filename = nullptr;
    module->nm_register_func = registerFunc;
    module->nm_modname = moduleName;
    module->nm_priv = nullptr;

    napi_module_register(module.get());

    const std::lock_guard<std::mutex> lock(registeredModulesMutex());
    registeredModules().push_back(std::move(module));
}

}
