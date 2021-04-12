/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <gtest/gtest.h>
#include <tvm/runtime/crt/stack_allocator.h>

/*
 * Tests allocations are properly aligned when allocated
 */
TEST(AOTMemory, Allocate) {
  static uint8_t model_memory[80];
  tvm_workspace_t tvm_runtime_workspace;

  StackMemoryManager_Init(&tvm_runtime_workspace, model_memory, 80);

  void* block_one = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_one, &model_memory[0]);

  void* block_two = StackMemoryManager_Allocate(&tvm_runtime_workspace, 2);
  ASSERT_EQ(block_two, &model_memory[16]);

  void* two_blocks = StackMemoryManager_Allocate(&tvm_runtime_workspace, 24);
  ASSERT_EQ(two_blocks, &model_memory[32]);

  void* block_three = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_three, &model_memory[64]);
}

/*
 * Tests resetting the stack after dealloc
 */
TEST(AOTMemory, Free) {
  static uint8_t model_memory[80];
  tvm_workspace_t tvm_runtime_workspace;
  StackMemoryManager_Init(&tvm_runtime_workspace, model_memory, 80);

  void* block_one = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_one, &model_memory[0]);

  void* block_two = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_two, &model_memory[16]);
  ASSERT_EQ(0, StackMemoryManager_Free(&tvm_runtime_workspace, block_two));

  void* two_blocks = StackMemoryManager_Allocate(&tvm_runtime_workspace, 2);
  ASSERT_EQ(two_blocks, &model_memory[16]);
  ASSERT_EQ(0, StackMemoryManager_Free(&tvm_runtime_workspace, two_blocks));

  void* block_three = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_three, &model_memory[16]);
}

/*
 * Tests we return NULL if we over allocate
 */
TEST(AOTMemory, OverAllocate) {
  static uint8_t model_memory[72];
  tvm_workspace_t tvm_runtime_workspace;
  StackMemoryManager_Init(&tvm_runtime_workspace, model_memory, 80);

  void* block_one = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_one, &model_memory[0]);

  void* block_two = StackMemoryManager_Allocate(&tvm_runtime_workspace, 1);
  ASSERT_EQ(block_two, &model_memory[16]);

  void* two_blocks = StackMemoryManager_Allocate(&tvm_runtime_workspace, 64);
  ASSERT_EQ(two_blocks, (void*)NULL);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  return RUN_ALL_TESTS();
}
