#include "sub_0000_tensors.h"

const TensorInfo sub_0000_tensors[] = {
  { "_split_1_command_stream", 2, 11980, "COMMAND_STREAM", 0xffffffff },
  { "_split_1_flash", 3, 250416, "MODEL", 0xffffffff },
  { "_split_1_scratch", 4, 221184, "ARENA", 0x0 },
  { "_split_1_scratch_fast", 5, 221184, "FAST_SCRATCH", 0x0 },
  { "serving_default_images_0", 6, 9216, "INPUT_TENSOR", 0x12000 },
  { "PartitionedCall_1_70211", 1, 180, "OUTPUT_TENSOR", 0x0 },
  { "PartitionedCall_0_70200", 0, 720, "OUTPUT_TENSOR", 0xc0 },
};

const size_t sub_0000_tensors_count = sizeof(sub_0000_tensors) / sizeof(sub_0000_tensors[0]);

// Addresses for each input and output buffer inside of the arena
const uint32_t sub_0000_address_serving_default_images_0 = 0x12000;
const uint32_t sub_0000_address_PartitionedCall_1_70211 = 0x0;
const uint32_t sub_0000_address_PartitionedCall_0_70200 = 0xc0;

