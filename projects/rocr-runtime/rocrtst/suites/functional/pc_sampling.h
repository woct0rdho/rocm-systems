/*
* Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
*
* SPDX-License-Identifier: MIT
*/
#ifndef ROCRTST_SUITES_FUNCTIONAL_PC_SAMPLING_H_
#define ROCRTST_SUITES_FUNCTIONAL_PC_SAMPLING_H_

#include "common/base_rocr.h"
#include "hsa/hsa.h"
#include "suites/test_common/test_base.h"

class PcSamplingTest : public TestBase {
 public:
  PcSamplingTest();

  virtual ~PcSamplingTest();

  virtual void SetUp();

  virtual void Run();

  virtual void Close();

  virtual void DisplayResults() const;

  virtual void DisplayTestInfo(void);

  void ExtensionAndConfigTest(void);

  void LifecycleTest(void);
};

#endif  // ROCRTST_SUITES_FUNCTIONAL_PC_SAMPLING_H_
