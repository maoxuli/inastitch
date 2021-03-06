// Copyright (C) 2020 Inatech srl
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// GLM includes:
#include <glm/glm.hpp>

// Taocpp/json includes:
#include <tao/json.hpp>
#include <tao/json/contrib/traits.hpp>
// Note: see taocpp documentation at
// https://github.com/taocpp/json/blob/master/doc/Common-Use-Cases.md

// TODO: forward declare types?

namespace inastitch {
namespace json {


void glmMat4ToJson(const glm::mat4 &mat, tao::json::basic_value<tao::json::traits>::array_t &array);
void glmMat3ToJson(const glm::mat3 &mat, tao::json::basic_value<tao::json::traits>::array_t &array);
void jsonToGlmMat4(const std::vector<float> &array, glm::mat4 &mat);
void jsonToGlmMat3(const std::vector<float> &array, glm::mat3 &mat);


} // namespace json
} // namespace inastitch
