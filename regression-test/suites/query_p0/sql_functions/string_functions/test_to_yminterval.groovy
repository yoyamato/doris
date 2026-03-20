// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

suite("test_to_yminterval") {
    qt_basic """select to_yminterval('01-02')"""
    qt_sign_and_padding """select to_yminterval('-10-3')"""
    qt_trimmed """select to_yminterval('  +123-11  ')"""
    qt_invalid_month """select to_yminterval('10-12')"""
    qt_invalid_format """select to_yminterval('abc')"""
    qt_null """select to_yminterval(null)"""
}
