/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "xla/service/gpu/command_buffer_scheduling.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/ir/hlo_schedule.h"
#include "xla/service/hlo_parser.h"
#include "xla/tests/filecheck.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/tests/verified_hlo_module.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/status.h"
#include "tsl/platform/statusor.h"

namespace xla::gpu {
namespace {

class CommandBufferSchedulingTest : public HloTestBase {
 public:
  // Use CUDA 12.3 version for testing as it has all the features we rely on.
  static constexpr int32_t kCudaVersion = 12030;

  const auto& gpu_comp() {
    return backend()
        .default_stream_executor()
        ->GetDeviceDescription()
        .gpu_compute_capability();
  }

  DebugOptions GetDebugOptionsForTest() override {
    auto debug_options = HloTestBase::GetDebugOptionsForTest();
    debug_options.add_xla_gpu_enable_command_buffer(DebugOptions::FUSION);
    debug_options.add_xla_gpu_enable_command_buffer(DebugOptions::WHILE);
    debug_options.add_xla_gpu_enable_command_buffer(DebugOptions::NCCL);
    debug_options.set_xla_gpu_graph_min_graph_size(2);
    return debug_options;
  }
};

using CommandBuffer = CommandBufferScheduling::CommandBuffer;

TEST_F(CommandBufferSchedulingTest, SingleCommandBuffer) {
  const char* hlo = R"(
      HloModule TestModule, is_scheduled=true

      %fused_computation (param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.1 (param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      ENTRY %main (a: s32[], b: s32[]) -> s32[] {
        %a = s32[] parameter(0)
        %b = s32[] parameter(1)
        %fusion = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation
        %fusion.1 = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation.1
        ROOT %custom-call = s32[] custom-call(s32[] %fusion, s32[] %fusion.1), custom_call_target="some target"
      })";

  const char* expected = R"(
// CHECK: %command_buffer ([[P0:.+]]: s32[], [[P1:.+]]: s32[]) -> (s32[], s32[]) {
// CHECK:   %[[P0]] = s32[] parameter(0)
// CHECK:   %[[P1]] = s32[] parameter(1)
// CHECK:   %fusion.2 = s32[] fusion(%[[P0]], %[[P1]]), kind=kLoop, calls=%fused_computation
// CHECK:   %fusion.3 = s32[] fusion(%[[P0]], %[[P1]]), kind=kLoop, calls=%fused_computation.1
// CHECK:   ROOT %tuple = (s32[], s32[]) tuple(%fusion.2, %fusion.3)
// CHECK: }
//
// CHECK: ENTRY %main (a: s32[], b: s32[]) -> s32[] {
// CHECK:   %a = s32[] parameter(0)
// CHECK:   %b = s32[] parameter(1)
// CHECK:   %call = (s32[], s32[]) call(%a, %b), to_apply=%command_buffer
// CHECK:   %get-tuple-element = s32[] get-tuple-element(%call), index=0
// CHECK:   %get-tuple-element.1 = s32[] get-tuple-element(%call), index=1
// CHECK:   ROOT %custom-call = s32[] custom-call(%get-tuple-element, %get-tuple-element.1), custom_call_target="some target"
// CHECK: })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

TEST_F(CommandBufferSchedulingTest, MultipleCommandBuffers) {
  const char* hlo = R"(
      HloModule TestModule, is_scheduled=true

      %fused_computation(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.1(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.2(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.3(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      ENTRY %main (a: s32[], b: s32[], c: (s32[], s32[])) -> s32[] {
        %a = s32[] parameter(0)
        %b = s32[] parameter(1)
        %c = (s32[], s32[]) parameter(2)
        %fusion = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation
        %d = s32[] get-tuple-element((s32[], s32[]) %c), index=0
        %fusion.1 = s32[] fusion(s32[] %fusion, s32[] %d), kind=kLoop, calls=%fused_computation.1
        %e = s32[] get-tuple-element((s32[], s32[]) %c), index=1
        %custom-call = s32[] custom-call(s32[] %fusion.1, s32[] %e), custom_call_target="some target"
        %fusion.2 = s32[] fusion(s32[] %custom-call, s32[] %a), kind=kLoop, calls=%fused_computation.2
        %fusion.3 = s32[] fusion(s32[] %custom-call, s32[] %fusion.2), kind=kLoop, calls=%fused_computation.3
        ROOT %custom-call.1 = s32[] custom-call(s32[] %fusion.3), custom_call_target="some target"
      })";

  const char* expected = R"(
// CHECK:  %command_buffer ([[P0:.+]]: s32[], [[P1:.+]]: s32[], [[P2:.+]]: (s32[], s32[])) -> s32[] {
// CHECK:    %[[P0]] = s32[] parameter(0)
// CHECK:    %[[P1]] = s32[] parameter(1)
// CHECK:    %[[P2]] = (s32[], s32[]) parameter(2)
// CHECK:    %[[F0:.+]] = s32[] fusion(%[[P0]], %[[P1]]), kind=kLoop, calls=%fused_computation
// CHECK:    %[[V0:.+]] = s32[] get-tuple-element(%[[P2]]), index=0
// CHECK:    ROOT {{.*}} = s32[] fusion(%[[F0]], %[[V0]]), kind=kLoop, calls=%fused_computation.1
// CHECK:  }

// CHECK:  %command_buffer.1 ([[P0:.+]]: s32[], [[P1:.+]]: s32[]) -> s32[] {
// CHECK:    %[[P0]] = s32[] parameter(0)
// CHECK:    %[[P1]] = s32[] parameter(1)
// CHECK:    %[[F2:.+]] = s32[] fusion(%[[P0]], %[[P1]]), kind=kLoop, calls=%fused_computation.2
// CHECK:    ROOT {{.*}} = s32[] fusion(%[[P0]], %[[F2]]), kind=kLoop, calls=%fused_computation.3
// CHECK:  }

// CHECK:  ENTRY %main (a: s32[], b: s32[], c: (s32[], s32[])) -> s32[] {
// CHECK:    %a = s32[] parameter(0)
// CHECK:    %b = s32[] parameter(1)
// CHECK:    %c = (s32[], s32[]) parameter(2)
// CHECK:    %[[CMD0:.+]] = s32[] call(%a, %b, %c), to_apply=%command_buffer
// CHECK:    %e = s32[] get-tuple-element(%c), index=1
// CHECK:    %[[CALL:.+]] = s32[] custom-call(%[[CMD0]], %e), custom_call_target="some target"
// CHECK:    %[[CMD1:.+]] = s32[] call(%[[CALL]], %a), to_apply=%command_buffer.1
// CHECK:    ROOT {{.*}} = s32[] custom-call(%[[CMD1]]), custom_call_target="some target"
// CHECK:  })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

TEST_F(CommandBufferSchedulingTest, AsyncStartFollowedByDone) {
  const char* hlo = R"(
    HloModule TestModule, is_scheduled=true

    %add (p0: s32[4], p1: s32[4]) -> s32[4] {
      %p0 = s32[4] parameter(0)
      %p1 = s32[4] parameter(1)
      ROOT %add = s32[4] add(s32[4] %p0, s32[4] %p1)
    }

    ENTRY %main (a: s32[4]) -> s32[4] {
      %a = s32[4] parameter(0)
      %start = s32[4]{0} all-reduce-start(s32[4]{0} %a),
        replica_groups={{0,1}}, to_apply=%add,
        backend_config={"is_sync":true,"no_parallel_custom_call":false}
      ROOT %done = s32[4]{0} all-reduce-done(s32[4]{0} %start)
    })";

  const char* expected = R"(
    CHECK: %command_buffer ([[P0:.+]]: s32[4]) -> s32[4] {
    CHECK:   %[[P0]] = s32[4]{0} parameter(0)
    CHECK:   %[[START:.+]] = s32[4]{0} all-reduce-start(%[[P0]])
    CHECK:   ROOT %[[DONE:.+]] = s32[4]{0} all-reduce-done(%[[START]])
    CHECK: }

    CHECK: ENTRY %main (a: s32[4]) -> s32[4] {
    CHECK:   %[[A:.+]] = s32[4]{0} parameter(0)
    CHECK:   ROOT %[[CALL:.+]] = s32[4]{0} call(%[[A]]),
    CHECL:     to_apply=%command_buffer
    CHECK: })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

TEST_F(CommandBufferSchedulingTest, CollectCommandBufferSequence) {
  const char* hlo = R"(
      HloModule TestModule, is_scheduled=true

      %fused_computation(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.1(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.2(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.3(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      ENTRY %main (a: s32[], b: s32[], c: (s32[], s32[])) -> s32[] {
        %a = s32[] parameter(0)
        %b = s32[] parameter(1)
        %c = (s32[], s32[]) parameter(2)
        %fusion = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation
        %d = s32[] get-tuple-element((s32[], s32[]) %c), index=0
        %fusion.1 = s32[] fusion(s32[] %fusion, s32[] %d), kind=kLoop, calls=%fused_computation.1
        %e = s32[] get-tuple-element((s32[], s32[]) %c), index=1
        %custom-call = s32[] custom-call(s32[] %fusion.1, s32[] %e), custom_call_target="some target"
        %fusion.2 = s32[] fusion(s32[] %custom-call, s32[] %a), kind=kLoop, calls=%fused_computation.2
        ROOT %fusion.3 = s32[] fusion(s32[] %custom-call, s32[] %fusion.2), kind=kLoop, calls=%fused_computation.3
      })";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<VerifiedHloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstructionSequence seq;
  for (HloInstruction* x : module->entry_computation()->instructions()) {
    seq.push_back(x);
  }
  EXPECT_EQ(seq.size(), 10);

  CommandBufferScheduling::CommandBufferConfig config;
  config.insert(DebugOptions::FUSION);

  std::vector<HloInstructionSequence> command_buffer_sequences =
      CommandBufferScheduling::CollectCommandBufferSequences(seq, config);
  EXPECT_EQ(command_buffer_sequences.size(), 2);

  std::vector<HloInstruction*> seq_0 =
      command_buffer_sequences[0].instructions();
  EXPECT_EQ(seq_0.size(), 3);
  EXPECT_EQ(seq_0[0]->opcode(), HloOpcode::kFusion);
  EXPECT_EQ(seq_0[1]->opcode(), HloOpcode::kGetTupleElement);
  EXPECT_EQ(seq_0[2]->opcode(), HloOpcode::kFusion);

  std::vector<HloInstruction*> seq_1 =
      command_buffer_sequences[1].instructions();
  EXPECT_EQ(seq_1.size(), 2);
  EXPECT_EQ(seq_1[0]->opcode(), HloOpcode::kFusion);
  EXPECT_EQ(seq_1[1]->opcode(), HloOpcode::kFusion);
}

TEST_F(CommandBufferSchedulingTest, MoveParametersToFront) {
  const char* hlo = R"(
      HloModule TestModule, is_scheduled=true

      %fused_computation (param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      %fused_computation.1 (param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      ENTRY %main (a: s32[], b: s32[], c: s32[]) -> s32[] {
        %a = s32[] parameter(0)
        %b = s32[] parameter(1)
        %fusion = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation
        %c = s32[] parameter(2)
        ROOT %fusion.1 = s32[] fusion(s32[] %a, s32[] %c), kind=kLoop, calls=%fused_computation.1
      })";

  const char* expected = R"(
// CHECK: ENTRY %main (a: s32[], b: s32[], c: s32[]) -> s32[] {
// CHECK:   %a = s32[] parameter(0)
// CHECK:   %b = s32[] parameter(1)
// CHECK:   %c = s32[] parameter(2)
// CHECK:   %fusion = s32[] fusion(%a, %b), kind=kLoop, calls=%fused_computation
// CHECK:   ROOT %fusion.1 = s32[] fusion(%a, %c), kind=kLoop, calls=%fused_computation.1
// CHECK: })";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<VerifiedHloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  TF_ASSERT_OK(CommandBufferScheduling::MoveParametersAndConstantsToFront(
      module->entry_computation()));
  TF_ASSERT_OK_AND_ASSIGN(
      bool filecheck_matches,
      RunFileCheck(
          module->ToString(HloPrintOptions{}.set_print_operand_shape(false)),
          expected));
  EXPECT_TRUE(filecheck_matches);
}

TEST_F(CommandBufferSchedulingTest, PrepareCommandBuffer) {
  const char* hlo = R"(
      HloModule TestModule, is_scheduled=true

      %fused_computation(param_0: s32[], param_1: s32[]) -> (s32[], s32[]) {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %tuple = (s32[], s32[]) tuple(s32[] %p0, s32[] %p1)
      }

      %fused_computation.1(param_0: s32[], param_1: s32[]) -> s32[] {
        %p0 = s32[] parameter(0)
        %p1 = s32[] parameter(1)
        ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
      }

      ENTRY %main (a: s32[], b: s32[]) -> s32[] {
        %a = s32[] parameter(0)
        %b = s32[] custom-call(), custom_call_target="target"
        %fusion = (s32[], s32[]) fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation
        %d = s32[] get-tuple-element((s32[], s32[]) %fusion), index=0
        %fusion.1 = s32[] fusion(s32[] %a, s32[] %d), kind=kLoop, calls=%fused_computation.1
        ROOT %custom-call = s32[] custom-call(s32[] %fusion.1, s32[] %d), custom_call_target="some target"
      })";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnUnverifiedModule(hlo));

  EXPECT_EQ(module->entry_computation()->instruction_count(), 6);
  std::vector<HloInstruction*> instructions;
  HloInstructionSequence seq;
  for (HloInstruction* inst : module->entry_computation()->instructions()) {
    if (inst->opcode() == HloOpcode::kFusion ||
        inst->opcode() == HloOpcode::kGetTupleElement) {
      seq.push_back(inst);
    }
    instructions.push_back(inst);
  }

  TF_ASSERT_OK_AND_ASSIGN(CommandBuffer command_buffer,
                          CommandBufferScheduling::PrepareCommandBuffer(seq));
  HloComputation* computation = module->AddComputationAndUnifyNamesAndIds(
      std::move(command_buffer.computation), false);

  const char* expected = R"(
// CHECK: %command_buffer ([[P0:.+]]: s32[], [[P1:.+]]: s32[]) -> (s32[], s32[]) {
// CHECK:  %[[P0]] = s32[] parameter(0)
// CHECK:  %[[P1]] = s32[] parameter(1)
// CHECK:  %fusion.2 = (s32[], s32[]) fusion(%[[P0]], %[[P1]]), kind=kLoop, calls=%fused_computation
// CHECK:  %[[V0:.+]] = s32[] get-tuple-element(%fusion.2), index=0
// CHECK:  %fusion.3 = s32[] fusion(%[[P0]], %[[V0]]), kind=kLoop, calls=%fused_computation.1
// CHECK:  ROOT {{.*}} = (s32[], s32[]) tuple(%[[V0]], %fusion.3)
// CHECK:})";

  TF_ASSERT_OK_AND_ASSIGN(
      bool filecheck_matches,
      RunFileCheck(computation->ToString(
                       HloPrintOptions{}.set_print_operand_shape(false)),
                   expected));
  EXPECT_TRUE(filecheck_matches);

  auto& arguments = command_buffer.arguments;
  ASSERT_EQ(arguments.size(), 2);
  EXPECT_EQ(arguments[0], instructions[0]);
  EXPECT_EQ(arguments[1], instructions[1]);

  auto& results = command_buffer.results;
  ASSERT_EQ(results.size(), 2);
  EXPECT_EQ(results[0], instructions[3]);
  EXPECT_EQ(results[1], instructions[4]);
}

TEST_F(CommandBufferSchedulingTest, ForwardControlDependencies) {
  const char* hlo = R"(
    HloModule TestModule, is_scheduled=true

    %fused_computation (param_0: s32[], param_1: s32[]) -> s32[] {
      %p0 = s32[] parameter(0)
      %p1 = s32[] parameter(1)
      ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
    }

    %fused_computation.1 (param_0: s32[], param_1: s32[]) -> s32[] {
      %p0 = s32[] parameter(0)
      %p1 = s32[] parameter(1)
      ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
    }

    %fused_computation.2 (param_0: s32[], param_1: s32[]) -> s32[] {
      %p0 = s32[] parameter(0)
      %p1 = s32[] parameter(1)
      ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
    }

    ENTRY %main (a: s32[], b: s32[]) -> s32[] {
      %a = s32[] parameter(0)
      %b = s32[] parameter(1)
      %custom-call = s32[] custom-call(), custom_call_target="some target"
      %fusion = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation, control-predecessors={%custom-call}
      %fusion.1 = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation.1, control-predecessors={%fusion}
      %custom-call.1 = s32[] custom-call(), custom_call_target="some target"
      %fusion.2 = s32[] fusion(s32[] %a, s32[] %b), kind=kLoop, calls=%fused_computation.2, control-predecessors={%fusion.1}
      ROOT %custom-call.2 = s32[] custom-call(s32[] %fusion.1, s32[] %fusion.2), custom_call_target="some target"
    })";

  const char* expected = R"(
    CHECK: %command_buffer ([[P0:.+]]: s32[], [[P1:.+]]: s32[]) -> s32[] {
    CHECK:   %[[P0]] = s32[] parameter(0)
    CHECK:   %[[P1]] = s32[] parameter(1)
    CHECK:   %[[F0:.+]] = s32[] fusion(%[[P0]], %[[P1]])
    CHECK:   ROOT {{.*}} = s32[] fusion(%[[P0]], %[[P1]]), {{.*}} control-predecessors={%[[F0]]}
    CHECK: }

    CHECK: ENTRY %main (a: s32[], b: s32[]) -> s32[] {
    CHECK:   %a = s32[] parameter(0)
    CHECK:   %b = s32[] parameter(1)
    CHECK:   %custom-call = s32[] custom-call(), custom_call_target="some target"
    CHECK:   %call = s32[] call(%a, %b), to_apply=%command_buffer, control-predecessors={%custom-call}
    CHECK:   %custom-call.1 = s32[] custom-call(), custom_call_target="some target"
    CHECK:   %[[F3:.+]] = s32[] fusion(%a, %b), kind=kLoop, calls=%fused_computation.2, control-predecessors={%call}
    CHECK:   ROOT %custom-call.2 = s32[] custom-call(%call, %[[F3]]), custom_call_target="some target"
    CHECK: })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

TEST_F(CommandBufferSchedulingTest, ForwardControlDependenciesToParams) {
  const char* hlo = R"(
    HloModule TestModule, is_scheduled=true

    %fused_computation.0 (p0: s32[], p1: s32[]) -> s32[] {
      %p0 = s32[] parameter(0)
      %p1 = s32[] parameter(1)
      ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
    }

    %fused_computation.1 (p0: s32[], p1: s32[]) -> s32[] {
      %p0 = s32[] parameter(0)
      %p1 = s32[] parameter(1)
      ROOT %add = s32[] add(s32[] %p0, s32[] %p1)
    }

    ENTRY %main (a: s32[], b: s32[]) -> s32[] {
      %a = s32[] parameter(0)
      %b = s32[] parameter(1)
      %custom-call = s32[] custom-call(), custom_call_target="some target"
      %fusion = s32[] fusion(s32[] %custom-call, s32[] %a), kind=kLoop, calls=%fused_computation.0, control-predecessors={%custom-call}
      ROOT %fusion.1 = s32[] fusion(s32[] %fusion, s32[] %b), kind=kLoop, calls=%fused_computation.1
    })";

  const char* expected = R"(
    CHECK: ENTRY %main (a: s32[], b: s32[]) -> s32[] {
    CHECK:   %a = s32[] parameter(0)
    CHECK:   %b = s32[] parameter(1)
    CHECK:   %[[CUSTOM_CALL:.+]] = s32[] custom-call(), custom_call_target="some target"
    CHECK:   ROOT {{.*}} call(%[[CUSTOM_CALL]], %a, %b), to_apply=%command_buffer, control-predecessors={%[[CUSTOM_CALL]]}
    CHECK: })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

TEST_F(CommandBufferSchedulingTest, WhileNotCommand) {
  const char* hlo = R"(
    HloModule TestModule, is_scheduled=true

    %fused_computation (param_0: f32[1]) -> f32[1] {
      %param_0 = f32[1]{0} parameter(0)
      ROOT %copy.5 = f32[1]{0} copy(f32[1]{0} %param_0)
    }

    %fused_computation.1 (param_0.1: f32[1], param_1: f32[1]) -> f32[1] {
      %param_0.1 = f32[1]{0} parameter(0)
      %param_1 = f32[1]{0} parameter(1)
      ROOT %add.2 = f32[1]{0} add(f32[1]{0} %param_0.1, f32[1]{0} %param_1)
    }

    %fused_computation.2 (param_0.2: f32[1], param_1.1: f32[1]) -> pred[1] {
      %param_0.2 = f32[1]{0} parameter(0)
      %param_1.1 = f32[1]{0} parameter(1)
      ROOT %compare.3 = pred[1]{0} compare(f32[1]{0} %param_0.2, f32[1]{0} %param_1.1), direction=LT
    }

    %fused_computation.3 (param_0.1: f32[1], param_1: f32[1]) -> f32[1] {
      %param_0.1 = f32[1]{0} parameter(0)
      %param_1 = f32[1]{0} parameter(1)
      ROOT %add.2 = f32[1]{0} add(f32[1]{0} %param_0.1, f32[1]{0} %param_1)
    }

    %body (Arg_.3: f32[1]) -> f32[1] {
      %constant_4 = f32[1]{0} constant({1})
      %Arg_.3 = f32[1]{0} parameter(0)
      %custom-call = s32[] custom-call(), custom_call_target="some target"
      %add = f32[1]{0} fusion(f32[1]{0} %Arg_.3, f32[1]{0} %constant_4), kind=kLoop, calls=%fused_computation.1, control-predecessors={%custom-call}
      ROOT %wrapped_add.1 = f32[1]{0} fusion(f32[1]{0} %add, f32[1]{0} %constant_4), kind=kLoop, calls=%fused_computation.3, control-predecessors={%custom-call}
    }

    %cond (Arg_.11: f32[1]) -> pred[] {
      %constant = f32[1]{0} constant({100})
      %Arg_.11 = f32[1]{0} parameter(0)
      %wrapped_compare.2 = pred[1]{0} fusion(f32[1]{0} %Arg_.11, f32[1]{0} %constant), kind=kLoop, calls=%fused_computation.2
      ROOT %bitcast = pred[] bitcast(pred[1]{0} %wrapped_compare.2)
    }

    ENTRY %main.18 (Arg_0.1: f32[1]) -> f32[] {
      %Arg_0.1 = f32[1]{0} parameter(0), sharding={replicated}
      %wrapped_copy.4 = f32[1]{0} fusion(f32[1]{0} %Arg_0.1), kind=kLoop, calls=%fused_computation
      %while.16 = f32[1]{0} while(f32[1]{0} %wrapped_copy.4), condition=%cond, body=%body
      ROOT %bitcast.1 = f32[] bitcast(f32[1]{0} %while.16)
    })";

  const char* expected = R"(
    CHECK: %command_buffer ([[P0:.+]]: f32[1], [[P1:.+]]: f32[1]) -> f32[1] {
    CHECK:   %[[P0]] = f32[1]{0} parameter(0)
    CHECK:   %[[P1]] = f32[1]{0} parameter(1)
    CHECK:   %[[ADD:.*]] = f32[1]{0} fusion(%[[P0]], %[[P1]]), kind=kLoop
    CHECK:   ROOT {{.*}} = f32[1]{0} fusion(%[[ADD]], %[[P1]]), kind=kLoop
    CHECK: }

    CHECK: %[[BODY:[a-z_0-9.]+]] ([[P0:.+]]: f32[1]) -> f32[1] {
    CHECK:   %[[C1:.*]] = f32[1]{0} constant({1})
    CHECK:   %[[P0]] = f32[1]{0} parameter(0)
    CHECK:   %[[CC:.*]] = s32[] custom-call(), custom_call_target="some target"
    CHECK:   ROOT %call = f32[1]{0} call(%[[P0]], %[[C1]]), to_apply=%command_buffer, control-predecessors={%[[CC]]}
    CHECK: }

    CHECK: ENTRY %[[MAIN:.+]] ([[ARG0:.+]]: f32[1]) -> f32[] {
    CHECK:   %[[ARG0]] = f32[1]{0} parameter(0)
    CHECK:   %[[COPY:.*]] = f32[1]{0} fusion(%[[ARG0]]), kind=kLoop
    CHECK:   %[[WHILE:.*]] = f32[1]{0} while(%[[COPY]]), condition=%[[COND:[a-z_0-9.]+]], body=%[[BODY]]
    CHECK:   ROOT %[[BC:.+]] = f32[] bitcast(%[[WHILE]])
    CHECK: })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

TEST_F(CommandBufferSchedulingTest, While) {
  const char* hlo = R"(
    HloModule TestModule, is_scheduled=true

    %fused_computation (param_0: f32[1]) -> f32[1] {
      %param_0 = f32[1]{0} parameter(0)
      ROOT %copy.5 = f32[1]{0} copy(f32[1]{0} %param_0)
    }

    %fused_computation.1 (param_0.1: f32[1], param_1: f32[1]) -> f32[1] {
      %param_0.1 = f32[1]{0} parameter(0)
      %param_1 = f32[1]{0} parameter(1)
      ROOT %add.2 = f32[1]{0} add(f32[1]{0} %param_0.1, f32[1]{0} %param_1)
    }

    %fused_computation.2 (param_0.2: f32[1], param_1.1: f32[1]) -> pred[1] {
      %param_0.2 = f32[1]{0} parameter(0)
      %param_1.1 = f32[1]{0} parameter(1)
      ROOT %compare.3 = pred[1]{0} compare(f32[1]{0} %param_0.2, f32[1]{0} %param_1.1), direction=LT
    }

    %body (Arg_.3: f32[1]) -> f32[1] {
      %constant_4 = f32[1]{0} constant({1})
      %Arg_.3 = f32[1]{0} parameter(0)
      ROOT %wrapped_add.1 = f32[1]{0} fusion(f32[1]{0} %Arg_.3, f32[1]{0} %constant_4), kind=kLoop, calls=%fused_computation.1
    }

    %cond (Arg_.11: f32[1]) -> pred[] {
      %constant = f32[1]{0} constant({100})
      %Arg_.11 = f32[1]{0} parameter(0)
      %wrapped_compare.2 = pred[1]{0} fusion(f32[1]{0} %Arg_.11, f32[1]{0} %constant), kind=kLoop, calls=%fused_computation.2
      ROOT %bitcast = pred[] bitcast(pred[1]{0} %wrapped_compare.2)
    }

    ENTRY %main.18 (Arg_0.1: f32[1]) -> f32[] {
      %Arg_0.1 = f32[1]{0} parameter(0), sharding={replicated}
      %wrapped_copy.4 = f32[1]{0} fusion(f32[1]{0} %Arg_0.1), kind=kLoop, calls=%fused_computation
      %while.16 = f32[1]{0} while(f32[1]{0} %wrapped_copy.4), condition=%cond, body=%body
      ROOT %bitcast.1 = f32[] bitcast(f32[1]{0} %while.16)
    })";

  const char* expected = R"(
    CHECK: %command_buffer ([[P0:.+]]: f32[1]) -> f32[1] {
    CHECK:   %[[P0]] = f32[1]{0} parameter(0)
    CHECK:   %[[COPY:.*]] = f32[1]{0} fusion(%[[P0]]), kind=kLoop
    CHECK:   ROOT {{.*}} = f32[1]{0} while(%[[COPY]]), condition=%[[COND:[a-z_0-9.]+]], body=%[[BODY:[a-z_0-9.]+]]
    CHECK: }

    CHECK: ENTRY %[[MAIN:.+]] ([[ARG0:.+]]: f32[1]) -> f32[] {
    CHECK:   %[[ARG0]] = f32[1]{0} parameter(0)
    CHECK:   %call = f32[1]{0} call(%[[ARG0]]), to_apply=%command_buffer
    CHECK:   ROOT %[[BC:.+]] = f32[] bitcast(%call)
    CHECK: })";

  RunAndFilecheckHloRewrite(
      hlo, CommandBufferScheduling(gpu_comp(), kCudaVersion, kCudaVersion),
      expected, [](HloModule* module) {
        EXPECT_TRUE(module->has_schedule());
        TF_CHECK_OK(module->schedule().Verify());
      });
}

}  // namespace

}  // namespace xla::gpu
