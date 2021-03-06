// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/test/unittest_test_suite.h"
#include "device/test/device_test_suite.h"

int main(int argc, char **argv) {
  return content::UnitTestTestSuite(new DeviceTestSuite(argc, argv)).Run();
}
