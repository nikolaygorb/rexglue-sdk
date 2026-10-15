// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 25226
; Schema: 0
               OpCapability Shader
               OpCapability StorageImageExtendedFormats
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID
               OpExecutionMode %main LocalSize 16 8 1
               OpSource GLSL 460
               OpSourceExtension "GL_EXT_control_flow_attributes"
               OpSourceExtension "GL_EXT_samplerless_texture_functions"
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpName %main "main"
               OpName %push_const_block_xe "push_const_block_xe"
               OpMemberName %push_const_block_xe 0 "xe_smaa_size"
               OpMemberName %push_const_block_xe 1 "xe_smaa_size_inv"
               OpName %push_consts_xe "push_consts_xe"
               OpName %xe_smaa_sampler_linear "xe_smaa_sampler_linear"
               OpName %gl_GlobalInvocationID "gl_GlobalInvocationID"
               OpName %xe_smaa_color "xe_smaa_color"
               OpName %xe_smaa_weights "xe_smaa_weights"
               OpName %xe_smaa_dest "xe_smaa_dest"
               OpDecorate %push_const_block_xe Block
               OpMemberDecorate %push_const_block_xe 0 Offset 0
               OpMemberDecorate %push_const_block_xe 1 Offset 8
               OpDecorate %xe_smaa_sampler_linear Binding 0
               OpDecorate %xe_smaa_sampler_linear DescriptorSet 0
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %xe_smaa_color Binding 0
               OpDecorate %xe_smaa_color DescriptorSet 1
               OpDecorate %xe_smaa_weights Binding 1
               OpDecorate %xe_smaa_weights DescriptorSet 1
               OpDecorate %xe_smaa_dest NonReadable
               OpDecorate %xe_smaa_dest Binding 0
               OpDecorate %xe_smaa_dest DescriptorSet 2
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
       %bool = OpTypeBool
     %v2bool = OpTypeVector %bool 2
      %float = OpTypeFloat 32
    %v2float = OpTypeVector %float 2
    %v4float = OpTypeVector %float 4
        %150 = OpTypeImage %float 2D 0 0 0 1 Unknown
%_ptr_UniformConstant_150 = OpTypePointer UniformConstant %150
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
%push_const_block_xe = OpTypeStruct %v2uint %v2float
%_ptr_PushConstant_push_const_block_xe = OpTypePointer PushConstant %push_const_block_xe
%push_consts_xe = OpVariable %_ptr_PushConstant_push_const_block_xe PushConstant
        %int = OpTypeInt 32 1
      %int_1 = OpConstant %int 1
%_ptr_PushConstant_v2float = OpTypePointer PushConstant %v2float
      %int_0 = OpConstant %int 0
%_ptr_PushConstant_v2uint = OpTypePointer PushConstant %v2uint
    %float_1 = OpConstant %float 1
    %float_0 = OpConstant %float 0
       %1416 = OpConstantComposite %v4float %float_1 %float_0 %float_0 %float_1
        %508 = OpTypeSampler
%_ptr_UniformConstant_508 = OpTypePointer UniformConstant %508
%xe_smaa_sampler_linear = OpVariable %_ptr_UniformConstant_508 UniformConstant
        %510 = OpTypeSampledImage %150
       %1284 = OpConstantComposite %v4float %float_1 %float_1 %float_1 %float_1
%float_9_99999975en06 = OpConstant %float 9.99999975e-06
        %768 = OpConstantComposite %v2float %float_1 %float_1
  %float_0_5 = OpConstant %float 0.5
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
%xe_smaa_color = OpVariable %_ptr_UniformConstant_150 UniformConstant
%xe_smaa_weights = OpVariable %_ptr_UniformConstant_150 UniformConstant
        %166 = OpTypeImage %float 2D 0 0 0 2 Rgb10A2
%_ptr_UniformConstant_166 = OpTypePointer UniformConstant %166
%xe_smaa_dest = OpVariable %_ptr_UniformConstant_166 UniformConstant
      %v2int = OpTypeVector %int 2
    %uint_16 = OpConstant %uint 16
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_16 %uint_8 %uint_1
       %1566 = OpConstantComposite %v2float %float_0_5 %float_0_5
          %2 = OpUndef %float
       %main = OpFunction %void None %1282
      %15110 = OpLabel
               OpSelectionMerge %21573 None
               OpSwitch %uint_0 %12914
      %12914 = OpLabel
      %13761 = OpLoad %v3uint %gl_GlobalInvocationID
      %21717 = OpVectorShuffle %v2uint %13761 %13761 0 1
       %7760 = OpAccessChain %_ptr_PushConstant_v2uint %push_consts_xe %int_0
      %13378 = OpLoad %v2uint %7760
      %23437 = OpUGreaterThanEqual %v2bool %21717 %13378
      %23076 = OpAny %bool %23437
               OpSelectionMerge %9574 DontFlatten
               OpBranchConditional %23076 %21992 %9574
      %21992 = OpLabel
               OpBranch %21573
       %9574 = OpLabel
      %17831 = OpConvertUToF %v2float %21717
      %24187 = OpFAdd %v2float %17831 %1566
      %11863 = OpAccessChain %_ptr_PushConstant_v2float %push_consts_xe %int_1
      %20667 = OpLoad %v2float %11863
       %6476 = OpFMul %v2float %24187 %20667
       %9348 = OpCompositeExtract %float %20667 0
      %10991 = OpCompositeExtract %float %20667 1
      %16161 = OpCompositeConstruct %v4float %9348 %10991 %2 %2
      %23380 = OpVectorShuffle %v4float %16161 %16161 0 1 0 1
      %21527 = OpVectorShuffle %v4float %6476 %6476 0 1 0 1
      %24336 = OpExtInst %v4float %1 Fma %23380 %1416 %21527
               OpSelectionMerge %9007 None
               OpSwitch %uint_0 %12572
      %12572 = OpLabel
      %17274 = OpLoad %150 %xe_smaa_weights
      %19789 = OpLoad %508 %xe_smaa_sampler_linear
      %13837 = OpSampledImage %510 %17274 %19789
      %11917 = OpVectorShuffle %v2float %24336 %24336 0 1
       %8444 = OpImageSampleExplicitLod %v4float %13837 %11917 Lod %float_0
      %13541 = OpCompositeExtract %float %8444 3
      %24924 = OpLoad %150 %xe_smaa_weights
      %10715 = OpLoad %508 %xe_smaa_sampler_linear
      %13838 = OpSampledImage %510 %24924 %10715
      %11918 = OpVectorShuffle %v2float %24336 %24336 2 3
       %8445 = OpImageSampleExplicitLod %v4float %13838 %11918 Lod %float_0
      %13542 = OpCompositeExtract %float %8445 1
      %24925 = OpLoad %150 %xe_smaa_weights
      %10886 = OpLoad %508 %xe_smaa_sampler_linear
      %12147 = OpSampledImage %510 %24925 %10886
      %15675 = OpImageSampleExplicitLod %v4float %12147 %6476 Lod %float_0
      %12859 = OpCompositeExtract %float %15675 0
      %24137 = OpCompositeExtract %float %15675 2
       %6219 = OpCompositeConstruct %v4float %13541 %13542 %24137 %12859
       %9986 = OpDot %float %6219 %1284
      %12393 = OpFOrdLessThan %bool %9986 %float_9_99999975en06
               OpSelectionMerge %18581 DontFlatten
               OpBranchConditional %12393 %14458 %23054
      %23054 = OpLabel
      %17718 = OpExtInst %float %1 FMax %13541 %24137
      %23374 = OpExtInst %float %1 FMax %13542 %12859
      %16470 = OpFOrdGreaterThan %bool %17718 %23374
      %20883 = OpCompositeConstruct %v4float %float_0 %13542 %float_0 %12859
       %6537 = OpVectorShuffle %v2float %6219 %6219 1 3
      %20530 = OpVectorShuffle %v2float %20883 %20883 0 1
               OpSelectionMerge %21910 Flatten
               OpBranchConditional %16470 %12760 %21910
      %12760 = OpLabel
      %21521 = OpCompositeInsert %v2float %13541 %20530 0
               OpBranch %21910
      %21910 = OpLabel
      %10924 = OpPhi %v2float %20530 %23054 %21521 %12760
               OpSelectionMerge %18756 Flatten
               OpBranchConditional %16470 %12761 %18756
      %12761 = OpLabel
      %21522 = OpCompositeInsert %v2float %float_0 %10924 1
               OpBranch %18756
      %18756 = OpLabel
      %17360 = OpPhi %v2float %10924 %21910 %21522 %12761
      %23016 = OpCompositeExtract %float %17360 0
      %14586 = OpCompositeExtract %float %17360 1
      %17610 = OpVectorShuffle %v2float %20883 %20883 2 3
               OpSelectionMerge %21911 Flatten
               OpBranchConditional %16470 %12762 %21911
      %12762 = OpLabel
      %21523 = OpCompositeInsert %v2float %24137 %17610 0
               OpBranch %21911
      %21911 = OpLabel
      %10925 = OpPhi %v2float %17610 %18756 %21523 %12762
               OpSelectionMerge %18757 Flatten
               OpBranchConditional %16470 %12763 %18757
      %12763 = OpLabel
      %21524 = OpCompositeInsert %v2float %float_0 %10925 1
               OpBranch %18757
      %18757 = OpLabel
      %17361 = OpPhi %v2float %10925 %21911 %21524 %12763
      %23035 = OpCompositeExtract %float %17361 0
      %14413 = OpCompositeExtract %float %17361 1
       %9417 = OpCompositeConstruct %v4float %23016 %14586 %23035 %14413
               OpSelectionMerge %21912 Flatten
               OpBranchConditional %16470 %12764 %21912
      %12764 = OpLabel
      %21525 = OpCompositeInsert %v2float %13541 %6537 0
               OpBranch %21912
      %21912 = OpLabel
      %10926 = OpPhi %v2float %6537 %18757 %21525 %12764
               OpSelectionMerge %20029 Flatten
               OpBranchConditional %16470 %12765 %20029
      %12765 = OpLabel
      %21526 = OpCompositeInsert %v2float %24137 %10926 1
               OpBranch %20029
      %20029 = OpLabel
      %24821 = OpPhi %v2float %10926 %21912 %21526 %12765
      %21943 = OpDot %float %24821 %768
      %10614 = OpCompositeConstruct %v2float %21943 %21943
      %21826 = OpFDiv %v2float %24821 %10614
      %16229 = OpVectorShuffle %v2float %16161 %16161 0 1
      %21544 = OpFNegate %v2float %16229
       %6288 = OpCompositeExtract %float %21544 0
      %11386 = OpCompositeExtract %float %21544 1
       %7260 = OpCompositeConstruct %v4float %9348 %10991 %6288 %11386
      %12666 = OpExtInst %v4float %1 Fma %9417 %7260 %21527
      %10710 = OpCompositeExtract %float %21826 0
      %21297 = OpLoad %150 %xe_smaa_color
      %10716 = OpLoad %508 %xe_smaa_sampler_linear
      %13839 = OpSampledImage %510 %21297 %10716
      %13076 = OpVectorShuffle %v2float %12666 %12666 0 1
      %17342 = OpImageSampleExplicitLod %v4float %13839 %13076 Lod %float_0
      %25225 = OpVectorTimesScalar %v4float %17342 %10710
      %21996 = OpCompositeExtract %float %21826 1
      %10662 = OpLoad %150 %xe_smaa_color
      %10717 = OpLoad %508 %xe_smaa_sampler_linear
      %13840 = OpSampledImage %510 %10662 %10717
      %13077 = OpVectorShuffle %v2float %12666 %12666 2 3
      %18254 = OpImageSampleExplicitLod %v4float %13840 %13077 Lod %float_0
      %20493 = OpVectorTimesScalar %v4float %18254 %21996
      %19323 = OpFAdd %v4float %25225 %20493
               OpBranch %9007
      %14458 = OpLabel
      %15661 = OpLoad %150 %xe_smaa_color
      %19960 = OpLoad %508 %xe_smaa_sampler_linear
      %15339 = OpSampledImage %510 %15661 %19960
       %8855 = OpImageSampleExplicitLod %v4float %15339 %6476 Lod %float_0
               OpBranch %9007
      %18581 = OpLabel
               OpUnreachable
       %9007 = OpLabel
       %7405 = OpPhi %v4float %8855 %14458 %19323 %20029
       %7836 = OpLoad %166 %xe_smaa_dest
      %13802 = OpBitcast %v2int %21717
      %24522 = OpCompositeExtract %float %7405 0
      %13264 = OpCompositeExtract %float %7405 1
       %8175 = OpCompositeExtract %float %7405 2
      %15931 = OpCompositeConstruct %v4float %24522 %13264 %8175 %float_1
               OpImageWrite %7836 %13802 %15931
               OpBranch %21573
      %21573 = OpLabel
               OpReturn
               OpFunctionEnd
#endif

const uint32_t smaa_neighborhood_blending_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x0000628A, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000031, 0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E,
    0x00000000, 0x00000001, 0x0006000F, 0x00000005, 0x0000161F, 0x6E69616D, 0x00000000, 0x00000F48,
    0x00060010, 0x0000161F, 0x00000011, 0x00000010, 0x00000008, 0x00000001, 0x00030003, 0x00000002,
    0x000001CC, 0x00090004, 0x455F4C47, 0x635F5458, 0x72746E6F, 0x665F6C6F, 0x5F776F6C, 0x72747461,
    0x74756269, 0x00007365, 0x000B0004, 0x455F4C47, 0x735F5458, 0x6C706D61, 0x656C7265, 0x745F7373,
    0x75747865, 0x665F6572, 0x74636E75, 0x736E6F69, 0x00000000, 0x000A0004, 0x475F4C47, 0x4C474F4F,
    0x70635F45, 0x74735F70, 0x5F656C79, 0x656E696C, 0x7269645F, 0x69746365, 0x00006576, 0x00080004,
    0x475F4C47, 0x4C474F4F, 0x6E695F45, 0x64756C63, 0x69645F65, 0x74636572, 0x00657669, 0x00040005,
    0x0000161F, 0x6E69616D, 0x00000000, 0x00070005, 0x00000402, 0x68737570, 0x6E6F635F, 0x625F7473,
    0x6B636F6C, 0x0065785F, 0x00070006, 0x00000402, 0x00000000, 0x735F6578, 0x5F61616D, 0x657A6973,
    0x00000000, 0x00080006, 0x00000402, 0x00000001, 0x735F6578, 0x5F61616D, 0x657A6973, 0x766E695F,
    0x00000000, 0x00060005, 0x00000CE9, 0x68737570, 0x6E6F635F, 0x5F737473, 0x00006578, 0x00080005,
    0x000012E0, 0x735F6578, 0x5F61616D, 0x706D6173, 0x5F72656C, 0x656E696C, 0x00007261, 0x00080005,
    0x00000F48, 0x475F6C67, 0x61626F6C, 0x766E496C, 0x7461636F, 0x496E6F69, 0x00000044, 0x00060005,
    0x00001721, 0x735F6578, 0x5F61616D, 0x6F6C6F63, 0x00000072, 0x00060005, 0x00001113, 0x735F6578,
    0x5F61616D, 0x67696577, 0x00737468, 0x00060005, 0x000016FB, 0x735F6578, 0x5F61616D, 0x74736564,
    0x00000000, 0x00030047, 0x00000402, 0x00000002, 0x00050048, 0x00000402, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x00000402, 0x00000001, 0x00000023, 0x00000008, 0x00040047, 0x000012E0,
    0x00000021, 0x00000000, 0x00040047, 0x000012E0, 0x00000022, 0x00000000, 0x00040047, 0x00000F48,
    0x0000000B, 0x0000001C, 0x00040047, 0x00001721, 0x00000021, 0x00000000, 0x00040047, 0x00001721,
    0x00000022, 0x00000001, 0x00040047, 0x00001113, 0x00000021, 0x00000001, 0x00040047, 0x00001113,
    0x00000022, 0x00000001, 0x00030047, 0x000016FB, 0x00000019, 0x00040047, 0x000016FB, 0x00000021,
    0x00000000, 0x00040047, 0x000016FB, 0x00000022, 0x00000002, 0x00040047, 0x00000B0F, 0x0000000B,
    0x00000019, 0x00020013, 0x00000008, 0x00030021, 0x00000502, 0x00000008, 0x00020014, 0x00000009,
    0x00040017, 0x0000000F, 0x00000009, 0x00000002, 0x00030016, 0x0000000D, 0x00000020, 0x00040017,
    0x00000013, 0x0000000D, 0x00000002, 0x00040017, 0x0000001D, 0x0000000D, 0x00000004, 0x00090019,
    0x00000096, 0x0000000D, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x00040020, 0x00000313, 0x00000000, 0x00000096, 0x00040015, 0x0000000B, 0x00000020, 0x00000000,
    0x00040017, 0x00000011, 0x0000000B, 0x00000002, 0x0004002B, 0x0000000B, 0x00000A0A, 0x00000000,
    0x0004002B, 0x0000000B, 0x00000A0D, 0x00000001, 0x0004001E, 0x00000402, 0x00000011, 0x00000013,
    0x00040020, 0x0000067F, 0x00000009, 0x00000402, 0x0004003B, 0x0000067F, 0x00000CE9, 0x00000009,
    0x00040015, 0x0000000C, 0x00000020, 0x00000001, 0x0004002B, 0x0000000C, 0x00000A0E, 0x00000001,
    0x00040020, 0x00000290, 0x00000009, 0x00000013, 0x0004002B, 0x0000000C, 0x00000A0B, 0x00000000,
    0x00040020, 0x0000028E, 0x00000009, 0x00000011, 0x0004002B, 0x0000000D, 0x0000008A, 0x3F800000,
    0x0004002B, 0x0000000D, 0x00000A0C, 0x00000000, 0x0007002C, 0x0000001D, 0x00000588, 0x0000008A,
    0x00000A0C, 0x00000A0C, 0x0000008A, 0x0002001A, 0x000001FC, 0x00040020, 0x00000479, 0x00000000,
    0x000001FC, 0x0004003B, 0x00000479, 0x000012E0, 0x00000000, 0x0003001B, 0x000001FE, 0x00000096,
    0x0007002C, 0x0000001D, 0x00000504, 0x0000008A, 0x0000008A, 0x0000008A, 0x0000008A, 0x0004002B,
    0x0000000D, 0x000007F1, 0x3727C5AC, 0x0005002C, 0x00000013, 0x00000300, 0x0000008A, 0x0000008A,
    0x0004002B, 0x0000000D, 0x000000FC, 0x3F000000, 0x00040017, 0x00000014, 0x0000000B, 0x00000003,
    0x00040020, 0x00000291, 0x00000001, 0x00000014, 0x0004003B, 0x00000291, 0x00000F48, 0x00000001,
    0x0004003B, 0x00000313, 0x00001721, 0x00000000, 0x0004003B, 0x00000313, 0x00001113, 0x00000000,
    0x00090019, 0x000000A6, 0x0000000D, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002,
    0x0000000B, 0x00040020, 0x00000323, 0x00000000, 0x000000A6, 0x0004003B, 0x00000323, 0x000016FB,
    0x00000000, 0x00040017, 0x00000012, 0x0000000C, 0x00000002, 0x0004002B, 0x0000000B, 0x00000A3A,
    0x00000010, 0x0004002B, 0x0000000B, 0x00000A22, 0x00000008, 0x0006002C, 0x00000014, 0x00000B0F,
    0x00000A3A, 0x00000A22, 0x00000A0D, 0x0005002C, 0x00000013, 0x0000061E, 0x000000FC, 0x000000FC,
    0x00030001, 0x0000000D, 0x00000002, 0x00050036, 0x00000008, 0x0000161F, 0x00000000, 0x00000502,
    0x000200F8, 0x00003B06, 0x000300F7, 0x00005445, 0x00000000, 0x000300FB, 0x00000A0A, 0x00003272,
    0x000200F8, 0x00003272, 0x0004003D, 0x00000014, 0x000035C1, 0x00000F48, 0x0007004F, 0x00000011,
    0x000054D5, 0x000035C1, 0x000035C1, 0x00000000, 0x00000001, 0x00050041, 0x0000028E, 0x00001E50,
    0x00000CE9, 0x00000A0B, 0x0004003D, 0x00000011, 0x00003442, 0x00001E50, 0x000500AE, 0x0000000F,
    0x00005B8D, 0x000054D5, 0x00003442, 0x0004009A, 0x00000009, 0x00005A24, 0x00005B8D, 0x000300F7,
    0x00002566, 0x00000002, 0x000400FA, 0x00005A24, 0x000055E8, 0x00002566, 0x000200F8, 0x000055E8,
    0x000200F9, 0x00005445, 0x000200F8, 0x00002566, 0x00040070, 0x00000013, 0x000045A7, 0x000054D5,
    0x00050081, 0x00000013, 0x00005E7B, 0x000045A7, 0x0000061E, 0x00050041, 0x00000290, 0x00002E57,
    0x00000CE9, 0x00000A0E, 0x0004003D, 0x00000013, 0x000050BB, 0x00002E57, 0x00050085, 0x00000013,
    0x0000194C, 0x00005E7B, 0x000050BB, 0x00050051, 0x0000000D, 0x00002484, 0x000050BB, 0x00000000,
    0x00050051, 0x0000000D, 0x00002AEF, 0x000050BB, 0x00000001, 0x00070050, 0x0000001D, 0x00003F21,
    0x00002484, 0x00002AEF, 0x00000002, 0x00000002, 0x0009004F, 0x0000001D, 0x00005B54, 0x00003F21,
    0x00003F21, 0x00000000, 0x00000001, 0x00000000, 0x00000001, 0x0009004F, 0x0000001D, 0x00005417,
    0x0000194C, 0x0000194C, 0x00000000, 0x00000001, 0x00000000, 0x00000001, 0x0008000C, 0x0000001D,
    0x00005F10, 0x00000001, 0x00000032, 0x00005B54, 0x00000588, 0x00005417, 0x000300F7, 0x0000232F,
    0x00000000, 0x000300FB, 0x00000A0A, 0x0000311C, 0x000200F8, 0x0000311C, 0x0004003D, 0x00000096,
    0x0000437A, 0x00001113, 0x0004003D, 0x000001FC, 0x00004D4D, 0x000012E0, 0x00050056, 0x000001FE,
    0x0000360D, 0x0000437A, 0x00004D4D, 0x0007004F, 0x00000013, 0x00002E8D, 0x00005F10, 0x00005F10,
    0x00000000, 0x00000001, 0x00070058, 0x0000001D, 0x000020FC, 0x0000360D, 0x00002E8D, 0x00000002,
    0x00000A0C, 0x00050051, 0x0000000D, 0x000034E5, 0x000020FC, 0x00000003, 0x0004003D, 0x00000096,
    0x0000615C, 0x00001113, 0x0004003D, 0x000001FC, 0x000029DB, 0x000012E0, 0x00050056, 0x000001FE,
    0x0000360E, 0x0000615C, 0x000029DB, 0x0007004F, 0x00000013, 0x00002E8E, 0x00005F10, 0x00005F10,
    0x00000002, 0x00000003, 0x00070058, 0x0000001D, 0x000020FD, 0x0000360E, 0x00002E8E, 0x00000002,
    0x00000A0C, 0x00050051, 0x0000000D, 0x000034E6, 0x000020FD, 0x00000001, 0x0004003D, 0x00000096,
    0x0000615D, 0x00001113, 0x0004003D, 0x000001FC, 0x00002A86, 0x000012E0, 0x00050056, 0x000001FE,
    0x00002F73, 0x0000615D, 0x00002A86, 0x00070058, 0x0000001D, 0x00003D3B, 0x00002F73, 0x0000194C,
    0x00000002, 0x00000A0C, 0x00050051, 0x0000000D, 0x0000323B, 0x00003D3B, 0x00000000, 0x00050051,
    0x0000000D, 0x00005E49, 0x00003D3B, 0x00000002, 0x00070050, 0x0000001D, 0x0000184B, 0x000034E5,
    0x000034E6, 0x00005E49, 0x0000323B, 0x00050094, 0x0000000D, 0x00002702, 0x0000184B, 0x00000504,
    0x000500B8, 0x00000009, 0x00003069, 0x00002702, 0x000007F1, 0x000300F7, 0x00004895, 0x00000002,
    0x000400FA, 0x00003069, 0x0000387A, 0x00005A0E, 0x000200F8, 0x00005A0E, 0x0007000C, 0x0000000D,
    0x00004536, 0x00000001, 0x00000028, 0x000034E5, 0x00005E49, 0x0007000C, 0x0000000D, 0x00005B4E,
    0x00000001, 0x00000028, 0x000034E6, 0x0000323B, 0x000500BA, 0x00000009, 0x00004056, 0x00004536,
    0x00005B4E, 0x00070050, 0x0000001D, 0x00005193, 0x00000A0C, 0x000034E6, 0x00000A0C, 0x0000323B,
    0x0007004F, 0x00000013, 0x00001989, 0x0000184B, 0x0000184B, 0x00000001, 0x00000003, 0x0007004F,
    0x00000013, 0x00005032, 0x00005193, 0x00005193, 0x00000000, 0x00000001, 0x000300F7, 0x00005596,
    0x00000001, 0x000400FA, 0x00004056, 0x000031D8, 0x00005596, 0x000200F8, 0x000031D8, 0x00060052,
    0x00000013, 0x00005411, 0x000034E5, 0x00005032, 0x00000000, 0x000200F9, 0x00005596, 0x000200F8,
    0x00005596, 0x000700F5, 0x00000013, 0x00002AAC, 0x00005032, 0x00005A0E, 0x00005411, 0x000031D8,
    0x000300F7, 0x00004944, 0x00000001, 0x000400FA, 0x00004056, 0x000031D9, 0x00004944, 0x000200F8,
    0x000031D9, 0x00060052, 0x00000013, 0x00005412, 0x00000A0C, 0x00002AAC, 0x00000001, 0x000200F9,
    0x00004944, 0x000200F8, 0x00004944, 0x000700F5, 0x00000013, 0x000043D0, 0x00002AAC, 0x00005596,
    0x00005412, 0x000031D9, 0x00050051, 0x0000000D, 0x000059E8, 0x000043D0, 0x00000000, 0x00050051,
    0x0000000D, 0x000038FA, 0x000043D0, 0x00000001, 0x0007004F, 0x00000013, 0x000044CA, 0x00005193,
    0x00005193, 0x00000002, 0x00000003, 0x000300F7, 0x00005597, 0x00000001, 0x000400FA, 0x00004056,
    0x000031DA, 0x00005597, 0x000200F8, 0x000031DA, 0x00060052, 0x00000013, 0x00005413, 0x00005E49,
    0x000044CA, 0x00000000, 0x000200F9, 0x00005597, 0x000200F8, 0x00005597, 0x000700F5, 0x00000013,
    0x00002AAD, 0x000044CA, 0x00004944, 0x00005413, 0x000031DA, 0x000300F7, 0x00004945, 0x00000001,
    0x000400FA, 0x00004056, 0x000031DB, 0x00004945, 0x000200F8, 0x000031DB, 0x00060052, 0x00000013,
    0x00005414, 0x00000A0C, 0x00002AAD, 0x00000001, 0x000200F9, 0x00004945, 0x000200F8, 0x00004945,
    0x000700F5, 0x00000013, 0x000043D1, 0x00002AAD, 0x00005597, 0x00005414, 0x000031DB, 0x00050051,
    0x0000000D, 0x000059FB, 0x000043D1, 0x00000000, 0x00050051, 0x0000000D, 0x0000384D, 0x000043D1,
    0x00000001, 0x00070050, 0x0000001D, 0x000024C9, 0x000059E8, 0x000038FA, 0x000059FB, 0x0000384D,
    0x000300F7, 0x00005598, 0x00000001, 0x000400FA, 0x00004056, 0x000031DC, 0x00005598, 0x000200F8,
    0x000031DC, 0x00060052, 0x00000013, 0x00005415, 0x000034E5, 0x00001989, 0x00000000, 0x000200F9,
    0x00005598, 0x000200F8, 0x00005598, 0x000700F5, 0x00000013, 0x00002AAE, 0x00001989, 0x00004945,
    0x00005415, 0x000031DC, 0x000300F7, 0x00004E3D, 0x00000001, 0x000400FA, 0x00004056, 0x000031DD,
    0x00004E3D, 0x000200F8, 0x000031DD, 0x00060052, 0x00000013, 0x00005416, 0x00005E49, 0x00002AAE,
    0x00000001, 0x000200F9, 0x00004E3D, 0x000200F8, 0x00004E3D, 0x000700F5, 0x00000013, 0x000060F5,
    0x00002AAE, 0x00005598, 0x00005416, 0x000031DD, 0x00050094, 0x0000000D, 0x000055B7, 0x000060F5,
    0x00000300, 0x00050050, 0x00000013, 0x00002976, 0x000055B7, 0x000055B7, 0x00050088, 0x00000013,
    0x00005542, 0x000060F5, 0x00002976, 0x0007004F, 0x00000013, 0x00003F65, 0x00003F21, 0x00003F21,
    0x00000000, 0x00000001, 0x0004007F, 0x00000013, 0x00005428, 0x00003F65, 0x00050051, 0x0000000D,
    0x00001890, 0x00005428, 0x00000000, 0x00050051, 0x0000000D, 0x00002C7A, 0x00005428, 0x00000001,
    0x00070050, 0x0000001D, 0x00001C5C, 0x00002484, 0x00002AEF, 0x00001890, 0x00002C7A, 0x0008000C,
    0x0000001D, 0x0000317A, 0x00000001, 0x00000032, 0x000024C9, 0x00001C5C, 0x00005417, 0x00050051,
    0x0000000D, 0x000029D6, 0x00005542, 0x00000000, 0x0004003D, 0x00000096, 0x00005331, 0x00001721,
    0x0004003D, 0x000001FC, 0x000029DC, 0x000012E0, 0x00050056, 0x000001FE, 0x0000360F, 0x00005331,
    0x000029DC, 0x0007004F, 0x00000013, 0x00003314, 0x0000317A, 0x0000317A, 0x00000000, 0x00000001,
    0x00070058, 0x0000001D, 0x000043BE, 0x0000360F, 0x00003314, 0x00000002, 0x00000A0C, 0x0005008E,
    0x0000001D, 0x00006289, 0x000043BE, 0x000029D6, 0x00050051, 0x0000000D, 0x000055EC, 0x00005542,
    0x00000001, 0x0004003D, 0x00000096, 0x000029A6, 0x00001721, 0x0004003D, 0x000001FC, 0x000029DD,
    0x000012E0, 0x00050056, 0x000001FE, 0x00003610, 0x000029A6, 0x000029DD, 0x0007004F, 0x00000013,
    0x00003315, 0x0000317A, 0x0000317A, 0x00000002, 0x00000003, 0x00070058, 0x0000001D, 0x0000474E,
    0x00003610, 0x00003315, 0x00000002, 0x00000A0C, 0x0005008E, 0x0000001D, 0x0000500D, 0x0000474E,
    0x000055EC, 0x00050081, 0x0000001D, 0x00004B7B, 0x00006289, 0x0000500D, 0x000200F9, 0x0000232F,
    0x000200F8, 0x0000387A, 0x0004003D, 0x00000096, 0x00003D2D, 0x00001721, 0x0004003D, 0x000001FC,
    0x00004DF8, 0x000012E0, 0x00050056, 0x000001FE, 0x00003BEB, 0x00003D2D, 0x00004DF8, 0x00070058,
    0x0000001D, 0x00002297, 0x00003BEB, 0x0000194C, 0x00000002, 0x00000A0C, 0x000200F9, 0x0000232F,
    0x000200F8, 0x00004895, 0x000100FF, 0x000200F8, 0x0000232F, 0x000700F5, 0x0000001D, 0x00001CED,
    0x00002297, 0x0000387A, 0x00004B7B, 0x00004E3D, 0x0004003D, 0x000000A6, 0x00001E9C, 0x000016FB,
    0x0004007C, 0x00000012, 0x000035EA, 0x000054D5, 0x00050051, 0x0000000D, 0x00005FCA, 0x00001CED,
    0x00000000, 0x00050051, 0x0000000D, 0x000033D0, 0x00001CED, 0x00000001, 0x00050051, 0x0000000D,
    0x00001FEF, 0x00001CED, 0x00000002, 0x00070050, 0x0000001D, 0x00003E3B, 0x00005FCA, 0x000033D0,
    0x00001FEF, 0x0000008A, 0x00040063, 0x00001E9C, 0x000035EA, 0x00003E3B, 0x000200F9, 0x00005445,
    0x000200F8, 0x00005445, 0x000100FD, 0x00010038,
};
