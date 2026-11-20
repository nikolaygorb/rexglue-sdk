// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 25246
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %gl_FragCoord %out_color
               OpExecutionMode %main OriginUpperLeft
               OpSource GLSL 460
               OpSourceExtension "GL_EXT_samplerless_texture_functions"
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpName %main "main"
               OpName %Constants "Constants"
               OpMemberName %Constants 0 "dst_base"
               OpMemberName %Constants 1 "dst_pitch_tiles"
               OpMemberName %Constants 2 "dst_format"
               OpMemberName %Constants 3 "src_base"
               OpMemberName %Constants 4 "src_pitch_tiles"
               OpMemberName %Constants 5 "src_format"
               OpMemberName %Constants 6 "swap_halves"
               OpMemberName %Constants 7 "stencil_bit"
               OpMemberName %Constants 8 "scales"
               OpMemberName %Constants 9 "tile_width"
               OpMemberName %Constants 10 "tile_count"
               OpName %c "c"
               OpName %gl_FragCoord "gl_FragCoord"
               OpName %src_color "src_color"
               OpName %out_color "out_color"
               OpDecorate %Constants Block
               OpMemberDecorate %Constants 0 Offset 0
               OpMemberDecorate %Constants 1 Offset 4
               OpMemberDecorate %Constants 2 Offset 8
               OpMemberDecorate %Constants 3 Offset 12
               OpMemberDecorate %Constants 4 Offset 16
               OpMemberDecorate %Constants 5 Offset 20
               OpMemberDecorate %Constants 6 Offset 24
               OpMemberDecorate %Constants 7 Offset 28
               OpMemberDecorate %Constants 8 Offset 32
               OpMemberDecorate %Constants 9 Offset 36
               OpMemberDecorate %Constants 10 Offset 40
               OpDecorate %gl_FragCoord BuiltIn FragCoord
               OpDecorate %src_color Binding 0
               OpDecorate %src_color DescriptorSet 0
               OpDecorate %out_color Location 0
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
      %float = OpTypeFloat 32
       %uint = OpTypeInt 32 0
    %v4float = OpTypeVector %float 4
    %float_0 = OpConstant %float 0
%float_31_875 = OpConstant %float 31.875
%uint_1048576000 = OpConstant %uint 1048576000
       %bool = OpTypeBool
%uint_8388607 = OpConstant %uint 8388607
%uint_8388608 = OpConstant %uint 8388608
   %uint_125 = OpConstant %uint 125
        %int = OpTypeInt 32 1
     %int_23 = OpConstant %int 23
    %uint_24 = OpConstant %uint 24
%uint_3254779904 = OpConstant %uint 3254779904
 %uint_32767 = OpConstant %uint 32767
     %int_16 = OpConstant %int 16
     %uint_1 = OpConstant %uint 1
  %uint_1023 = OpConstant %uint 1023
   %uint_127 = OpConstant %uint 127
      %int_7 = OpConstant %int 7
     %uint_7 = OpConstant %uint 7
     %uint_0 = OpConstant %uint 0
    %float_1 = OpConstant %float 1
    %float_3 = OpConstant %float 3
     %v3uint = OpTypeVector %uint 3
    %v3float = OpTypeVector %float 3
 %float_1023 = OpConstant %float 1023
     %int_10 = OpConstant %int 10
     %uint_2 = OpConstant %uint 2
     %int_20 = OpConstant %int 20
     %int_30 = OpConstant %int 30
     %v2uint = OpTypeVector %uint 2
    %v2float = OpTypeVector %float 2
   %float_n1 = OpConstant %float -1
%float_32767 = OpConstant %float 32767
      %v2int = OpTypeVector %int 2
 %uint_65535 = OpConstant %uint 65535
     %v4uint = OpTypeVector %uint 4
  %float_255 = OpConstant %float 255
      %int_8 = OpConstant %int 8
     %int_24 = OpConstant %int 24
         %73 = OpConstantComposite %v2float %float_n1 %float_n1
   %uint_255 = OpConstant %uint 255
  %Constants = OpTypeStruct %uint %uint %uint %uint %uint %uint %uint %uint %uint %uint %uint
%_ptr_PushConstant_Constants = OpTypePointer PushConstant %Constants
          %c = OpVariable %_ptr_PushConstant_Constants PushConstant
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
%_ptr_Input_v4float = OpTypePointer Input %v4float
%gl_FragCoord = OpVariable %_ptr_Input_v4float Input
      %int_9 = OpConstant %int 9
    %uint_16 = OpConstant %uint 16
     %uint_4 = OpConstant %uint 4
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
      %int_6 = OpConstant %int 6
      %int_3 = OpConstant %int 3
      %int_4 = OpConstant %int 4
        %150 = OpTypeImage %float 2D 0 0 0 1 Unknown
%_ptr_UniformConstant_150 = OpTypePointer UniformConstant %150
  %src_color = OpVariable %_ptr_UniformConstant_150 UniformConstant
    %uint_40 = OpConstant %uint 40
%_ptr_Output_v4float = OpTypePointer Output %v4float
  %out_color = OpVariable %_ptr_Output_v4float Output
      %int_5 = OpConstant %int 5
      %int_2 = OpConstant %int 2
       %1891 = OpConstantComposite %v2uint %uint_4 %uint_4
       %1849 = OpConstantComposite %v2uint %uint_2 %uint_2
       %1828 = OpConstantComposite %v2uint %uint_1 %uint_1
       %2938 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0
       %1284 = OpConstantComposite %v4float %float_1 %float_1 %float_1 %float_1
       %2604 = OpConstantComposite %v3float %float_0 %float_0 %float_0
       %2584 = OpConstantComposite %v3float %float_1 %float_1 %float_1
        %768 = OpConstantComposite %v2float %float_1 %float_1
       %2015 = OpConstantComposite %v2uint %uint_65535 %uint_65535
       %2151 = OpConstantComposite %v2int %int_16 %int_16
%float_3_05185094en05 = OpConstant %float 3.05185094e-05
       %1669 = OpConstantComposite %v2float %float_3_05185094en05 %float_3_05185094en05
%float_0_0078125 = OpConstant %float 0.0078125
%float_0_001953125 = OpConstant %float 0.001953125
%float_0_333333343 = OpConstant %float 0.333333343
%float_0_000977517106 = OpConstant %float 0.000977517106
%float_0_00392156886 = OpConstant %float 0.00392156886
       %1203 = OpConstantComposite %v4float %float_0_00392156886 %float_0_00392156886 %float_0_00392156886 %float_0_00392156886
       %main = OpFunction %void None %1282
      %15110 = OpLabel
               OpSelectionMerge %19578 None
               OpSwitch %uint_0 %11880
      %11880 = OpLabel
      %24791 = OpAccessChain %_ptr_PushConstant_uint %c %int_8
      %14043 = OpLoad %uint %24791
      %17939 = OpBitwiseAnd %uint %14043 %uint_255
      %22596 = OpShiftRightLogical %uint %14043 %int_8
      %19590 = OpLoad %v4float %gl_FragCoord
       %9372 = OpVectorShuffle %v2float %19590 %19590 0 1
       %6778 = OpConvertFToU %v2uint %9372
      %18578 = OpAccessChain %_ptr_PushConstant_uint %c %int_9
      %13987 = OpLoad %uint %18578
       %9671 = OpCompositeConstruct %v2uint %13987 %uint_16
      %24141 = OpCompositeConstruct %v2uint %17939 %17939
      %16502 = OpIMul %v2uint %9671 %24141
      %10185 = OpUDiv %v2uint %16502 %1891
      %24744 = OpUDiv %v2uint %6778 %10185
      %20616 = OpAccessChain %_ptr_PushConstant_uint %c %int_0
      %11092 = OpLoad %uint %20616
      %21795 = OpCompositeExtract %uint %24744 1
       %6464 = OpAccessChain %_ptr_PushConstant_uint %c %int_1
      %15306 = OpLoad %uint %6464
       %6538 = OpIMul %uint %21795 %15306
       %6858 = OpIAdd %uint %11092 %6538
      %23925 = OpCompositeExtract %uint %24744 0
       %8789 = OpIAdd %uint %6858 %23925
       %9786 = OpAccessChain %_ptr_PushConstant_uint %c %int_10
      %23366 = OpLoad %uint %9786
      %23025 = OpUMod %uint %8789 %23366
      %14208 = OpIMul %v2uint %24744 %10185
      %19430 = OpISub %v2uint %6778 %14208
       %6711 = OpCompositeConstruct %v2uint %22596 %22596
      %25245 = OpIMul %v2uint %9671 %6711
       %7242 = OpUDiv %v2uint %25245 %1891
      %11458 = OpINotEqual %bool %22596 %17939
               OpSelectionMerge %21648 None
               OpBranchConditional %11458 %20882 %21648
      %20882 = OpLabel
      %12020 = OpIMul %v2uint %19430 %1849
      %23015 = OpIAdd %v2uint %12020 %1828
      %18780 = OpIMul %v2uint %23015 %6711
      %24580 = OpIMul %uint %17939 %uint_2
       %8052 = OpCompositeConstruct %v2uint %24580 %24580
      %11111 = OpUDiv %v2uint %18780 %8052
               OpBranch %21648
      %21648 = OpLabel
      %19748 = OpPhi %v2uint %19430 %11880 %11111 %20882
       %6744 = OpAccessChain %_ptr_PushConstant_uint %c %int_6
      %20703 = OpLoad %uint %6744
      %12913 = OpINotEqual %bool %20703 %uint_0
               OpSelectionMerge %19649 None
               OpBranchConditional %12913 %9741 %19649
       %9741 = OpLabel
      %19610 = OpCompositeExtract %uint %19748 0
      %10692 = OpCompositeExtract %uint %7242 0
      %21877 = OpShiftRightLogical %uint %10692 %int_1
       %8102 = OpIAdd %uint %19610 %21877
      %20008 = OpUMod %uint %8102 %10692
      %18037 = OpCompositeInsert %v2uint %20008 %19748 0
               OpBranch %19649
      %19649 = OpLabel
       %8925 = OpPhi %v2uint %19748 %21648 %18037 %9741
      %21791 = OpIAdd %uint %23025 %23366
      %13079 = OpAccessChain %_ptr_PushConstant_uint %c %int_3
      %24672 = OpLoad %uint %13079
      %21063 = OpISub %uint %21791 %24672
      %11398 = OpUMod %uint %21063 %23366
      %17707 = OpAccessChain %_ptr_PushConstant_uint %c %int_4
       %8363 = OpLoad %uint %17707
      %21729 = OpUMod %uint %11398 %8363
       %7460 = OpUDiv %uint %11398 %8363
      %19019 = OpCompositeConstruct %v2uint %21729 %7460
       %9545 = OpIMul %v2uint %19019 %7242
      %22855 = OpIAdd %v2uint %9545 %8925
      %20608 = OpBitcast %v2int %22855
       %9006 = OpLoad %150 %src_color
      %12501 = OpImageFetch %v4float %9006 %20608 Lod %int_0
      %21428 = OpIEqual %bool %13987 %uint_40
               OpSelectionMerge %8158 None
               OpBranchConditional %21428 %16220 %8158
      %16220 = OpLabel
               OpStore %out_color %12501
               OpBranch %19578
       %8158 = OpLabel
      %24090 = OpAccessChain %_ptr_PushConstant_uint %c %int_5
       %6822 = OpLoad %uint %24090
               OpSelectionMerge %11416 None
               OpSwitch %6822 %12647 2 %20099 10 %20099 3 %19753 12 %19753 4 %18665 6 %9667 14 %10309
      %10309 = OpLabel
      %10794 = OpCompositeExtract %float %12501 0
      %13507 = OpBitcast %uint %10794
               OpBranch %11416
       %9667 = OpLabel
      %23771 = OpVectorShuffle %v2float %12501 %12501 0 1
      %11064 = OpExtInst %uint %1 PackHalf2x16 %23771
               OpBranch %11416
      %18665 = OpLabel
       %8826 = OpVectorShuffle %v2float %12501 %12501 0 1
      %21507 = OpExtInst %v2float %1 FClamp %8826 %73 %768
      %21873 = OpVectorTimesScalar %v2float %21507 %float_32767
      %17912 = OpExtInst %v2float %1 Round %21873
      %21572 = OpConvertFToS %v2int %17912
      %11077 = OpBitcast %v2uint %21572
      %15303 = OpBitwiseAnd %v2uint %11077 %2015
       %8914 = OpCompositeExtract %uint %15303 0
       %7652 = OpCompositeExtract %uint %15303 1
      %14753 = OpShiftLeftLogical %uint %7652 %int_16
      %10809 = OpBitwiseOr %uint %8914 %14753
               OpBranch %11416
      %19753 = OpLabel
      %11169 = OpCompositeExtract %float %12501 0
       %8167 = OpExtInst %float %1 FClamp %11169 %float_0 %float_31_875
      %19733 = OpBitcast %uint %8167
      %17874 = OpULessThan %bool %19733 %uint_1048576000
               OpSelectionMerge %13422 None
               OpBranchConditional %17874 %14661 %23873
      %23873 = OpLabel
       %6992 = OpIAdd %uint %19733 %uint_3254779904
               OpBranch %13422
      %14661 = OpLabel
      %20425 = OpBitwiseAnd %uint %19733 %uint_8388607
      %19952 = OpBitwiseOr %uint %20425 %uint_8388608
      %11750 = OpShiftRightLogical %uint %19733 %int_23
      %12122 = OpISub %uint %uint_125 %11750
      %16181 = OpExtInst %uint %1 UMin %12122 %uint_24
      %25241 = OpShiftRightLogical %uint %19952 %16181
               OpBranch %13422
      %13422 = OpLabel
      %11376 = OpPhi %uint %25241 %14661 %6992 %23873
      %21167 = OpIAdd %uint %11376 %uint_32767
      %12860 = OpShiftRightLogical %uint %11376 %int_16
      %23677 = OpBitwiseAnd %uint %12860 %uint_1
       %9641 = OpIAdd %uint %21167 %23677
      %19767 = OpShiftRightLogical %uint %9641 %int_16
       %9322 = OpBitwiseAnd %uint %19767 %uint_1023
       %9269 = OpCompositeExtract %float %12501 1
      %16776 = OpExtInst %float %1 FClamp %9269 %float_0 %float_31_875
      %19734 = OpBitcast %uint %16776
      %17875 = OpULessThan %bool %19734 %uint_1048576000
               OpSelectionMerge %13423 None
               OpBranchConditional %17875 %14662 %23874
      %23874 = OpLabel
       %6993 = OpIAdd %uint %19734 %uint_3254779904
               OpBranch %13423
      %14662 = OpLabel
      %20426 = OpBitwiseAnd %uint %19734 %uint_8388607
      %19953 = OpBitwiseOr %uint %20426 %uint_8388608
      %11751 = OpShiftRightLogical %uint %19734 %int_23
      %12123 = OpISub %uint %uint_125 %11751
      %16182 = OpExtInst %uint %1 UMin %12123 %uint_24
      %25242 = OpShiftRightLogical %uint %19953 %16182
               OpBranch %13423
      %13423 = OpLabel
      %11377 = OpPhi %uint %25242 %14662 %6993 %23874
      %21168 = OpIAdd %uint %11377 %uint_32767
      %12861 = OpShiftRightLogical %uint %11377 %int_16
      %23678 = OpBitwiseAnd %uint %12861 %uint_1
       %9642 = OpIAdd %uint %21168 %23678
      %21952 = OpShiftRightLogical %uint %9642 %int_16
      %11970 = OpBitwiseAnd %uint %21952 %uint_1023
      %21886 = OpShiftLeftLogical %uint %11970 %int_10
      %14973 = OpBitwiseOr %uint %9322 %21886
      %11380 = OpCompositeExtract %float %12501 2
      %15163 = OpExtInst %float %1 FClamp %11380 %float_0 %float_31_875
      %19735 = OpBitcast %uint %15163
      %17876 = OpULessThan %bool %19735 %uint_1048576000
               OpSelectionMerge %13424 None
               OpBranchConditional %17876 %14663 %23875
      %23875 = OpLabel
       %6994 = OpIAdd %uint %19735 %uint_3254779904
               OpBranch %13424
      %14663 = OpLabel
      %20427 = OpBitwiseAnd %uint %19735 %uint_8388607
      %19954 = OpBitwiseOr %uint %20427 %uint_8388608
      %11752 = OpShiftRightLogical %uint %19735 %int_23
      %12124 = OpISub %uint %uint_125 %11752
      %16183 = OpExtInst %uint %1 UMin %12124 %uint_24
      %25243 = OpShiftRightLogical %uint %19954 %16183
               OpBranch %13424
      %13424 = OpLabel
      %11378 = OpPhi %uint %25243 %14663 %6994 %23875
      %21169 = OpIAdd %uint %11378 %uint_32767
      %12862 = OpShiftRightLogical %uint %11378 %int_16
      %23679 = OpBitwiseAnd %uint %12862 %uint_1
       %9643 = OpIAdd %uint %21169 %23679
      %21953 = OpShiftRightLogical %uint %9643 %int_16
      %11971 = OpBitwiseAnd %uint %21953 %uint_1023
      %21887 = OpShiftLeftLogical %uint %11971 %int_20
      %14974 = OpBitwiseOr %uint %14973 %21887
      %11551 = OpCompositeExtract %float %12501 3
      %10491 = OpExtInst %float %1 FClamp %11551 %float_0 %float_1
      %20920 = OpFMul %float %10491 %float_3
      %15537 = OpExtInst %float %1 Round %20920
      %19698 = OpConvertFToU %uint %15537
      %16169 = OpShiftLeftLogical %uint %19698 %int_30
      %14020 = OpBitwiseOr %uint %14974 %16169
               OpBranch %11416
      %20099 = OpLabel
       %8827 = OpVectorShuffle %v3float %12501 %12501 0 1 2
      %21508 = OpExtInst %v3float %1 FClamp %8827 %2604 %2584
      %21854 = OpVectorTimesScalar %v3float %21508 %float_1023
      %17268 = OpExtInst %v3float %1 Round %21854
      %15891 = OpConvertFToU %v3uint %17268
      %11261 = OpCompositeExtract %uint %15891 0
      %12251 = OpCompositeExtract %uint %15891 1
      %11561 = OpShiftLeftLogical %uint %12251 %int_10
      %19814 = OpBitwiseOr %uint %11261 %11561
      %21476 = OpCompositeExtract %uint %15891 2
       %8560 = OpShiftLeftLogical %uint %21476 %int_20
      %16361 = OpBitwiseOr %uint %19814 %8560
      %11552 = OpCompositeExtract %float %12501 3
      %10492 = OpExtInst %float %1 FClamp %11552 %float_0 %float_1
      %20921 = OpFMul %float %10492 %float_3
      %15538 = OpExtInst %float %1 Round %20921
      %19699 = OpConvertFToU %uint %15538
      %16170 = OpShiftLeftLogical %uint %19699 %int_30
      %14021 = OpBitwiseOr %uint %16361 %16170
               OpBranch %11416
      %12647 = OpLabel
      %24160 = OpExtInst %v4float %1 FClamp %12501 %2938 %1284
      %13470 = OpVectorTimesScalar %v4float %24160 %float_255
      %17269 = OpExtInst %v4float %1 Round %13470
      %15892 = OpConvertFToU %v4uint %17269
      %11262 = OpCompositeExtract %uint %15892 0
      %12252 = OpCompositeExtract %uint %15892 1
      %11562 = OpShiftLeftLogical %uint %12252 %int_8
      %19815 = OpBitwiseOr %uint %11262 %11562
      %21477 = OpCompositeExtract %uint %15892 2
       %8561 = OpShiftLeftLogical %uint %21477 %int_16
      %19816 = OpBitwiseOr %uint %19815 %8561
      %21478 = OpCompositeExtract %uint %15892 3
      %11753 = OpShiftLeftLogical %uint %21478 %int_24
      %10810 = OpBitwiseOr %uint %19816 %11753
               OpBranch %11416
      %11416 = OpLabel
      %19749 = OpPhi %uint %10810 %12647 %14021 %20099 %14020 %13424 %10809 %18665 %11064 %9667 %13507 %10309
       %8188 = OpAccessChain %_ptr_PushConstant_uint %c %int_2
       %7631 = OpLoad %uint %8188
               OpSelectionMerge %24013 None
               OpSwitch %7631 %6620 2 %6619 10 %6619 3 %8272 12 %8272 4 %7366 6 %16738 14 %21105
      %21105 = OpLabel
       %9385 = OpBitcast %float %19749
      %19114 = OpCompositeConstruct %v4float %9385 %float_0 %float_0 %float_0
               OpBranch %24013
      %16738 = OpLabel
       %8759 = OpExtInst %v2float %1 UnpackHalf2x16 %19749
      %12699 = OpCompositeExtract %float %8759 0
      %20670 = OpCompositeExtract %float %8759 1
       %9033 = OpCompositeConstruct %v4float %12699 %20670 %float_0 %float_0
               OpBranch %24013
       %7366 = OpLabel
      %18161 = OpShiftLeftLogical %uint %19749 %int_16
       %7062 = OpBitcast %int %18161
      %20477 = OpBitcast %int %19749
      %22402 = OpCompositeConstruct %v2int %7062 %20477
      %22421 = OpShiftRightArithmetic %v2int %22402 %2151
      %15461 = OpConvertSToF %v2float %22421
      %15700 = OpFMul %v2float %15461 %1669
      %20154 = OpExtInst %v2float %1 FMax %15700 %73
      %20300 = OpCompositeExtract %float %20154 0
      %18764 = OpCompositeExtract %float %20154 1
       %9034 = OpCompositeConstruct %v4float %20300 %18764 %float_0 %float_0
               OpBranch %24013
       %8272 = OpLabel
      %20079 = OpBitwiseAnd %uint %19749 %uint_1023
      %23948 = OpBitwiseAnd %uint %19749 %uint_127
      %21350 = OpShiftRightLogical %uint %20079 %int_7
      %13638 = OpBitwiseAnd %uint %21350 %uint_7
      %19684 = OpIEqual %bool %13638 %uint_0
               OpSelectionMerge %7450 None
               OpBranchConditional %19684 %10229 %24437
      %24437 = OpLabel
      %22390 = OpConvertUToF %float %23948
      %19454 = OpFMul %float %22390 %float_0_0078125
      %17799 = OpFAdd %float %float_1 %19454
      %13697 = OpConvertUToF %float %13638
      %17869 = OpFSub %float %13697 %float_3
      %22591 = OpExtInst %float %1 Exp2 %17869
       %8923 = OpFMul %float %17799 %22591
               OpBranch %7450
      %10229 = OpLabel
      %20635 = OpConvertUToF %float %23948
      %20349 = OpFMul %float %20635 %float_0_001953125
               OpBranch %7450
       %7450 = OpLabel
      %19124 = OpPhi %float %20349 %10229 %8923 %24437
       %7095 = OpShiftRightLogical %uint %19749 %int_10
      %12428 = OpBitwiseAnd %uint %7095 %uint_1023
      %19139 = OpBitwiseAnd %uint %7095 %uint_127
      %21351 = OpShiftRightLogical %uint %12428 %int_7
      %13639 = OpBitwiseAnd %uint %21351 %uint_7
      %19685 = OpIEqual %bool %13639 %uint_0
               OpSelectionMerge %7451 None
               OpBranchConditional %19685 %10230 %24438
      %24438 = OpLabel
      %22391 = OpConvertUToF %float %19139
      %19455 = OpFMul %float %22391 %float_0_0078125
      %17800 = OpFAdd %float %float_1 %19455
      %13698 = OpConvertUToF %float %13639
      %17870 = OpFSub %float %13698 %float_3
      %22592 = OpExtInst %float %1 Exp2 %17870
       %8924 = OpFMul %float %17800 %22592
               OpBranch %7451
      %10230 = OpLabel
      %20636 = OpConvertUToF %float %19139
      %20350 = OpFMul %float %20636 %float_0_001953125
               OpBranch %7451
       %7451 = OpLabel
      %19125 = OpPhi %float %20350 %10230 %8924 %24438
       %7096 = OpShiftRightLogical %uint %19749 %int_20
      %12429 = OpBitwiseAnd %uint %7096 %uint_1023
      %19140 = OpBitwiseAnd %uint %7096 %uint_127
      %21352 = OpShiftRightLogical %uint %12429 %int_7
      %13640 = OpBitwiseAnd %uint %21352 %uint_7
      %19686 = OpIEqual %bool %13640 %uint_0
               OpSelectionMerge %7452 None
               OpBranchConditional %19686 %10231 %24439
      %24439 = OpLabel
      %22392 = OpConvertUToF %float %19140
      %19456 = OpFMul %float %22392 %float_0_0078125
      %17801 = OpFAdd %float %float_1 %19456
      %13699 = OpConvertUToF %float %13640
      %17871 = OpFSub %float %13699 %float_3
      %22593 = OpExtInst %float %1 Exp2 %17871
       %8926 = OpFMul %float %17801 %22593
               OpBranch %7452
      %10231 = OpLabel
      %20637 = OpConvertUToF %float %19140
      %20351 = OpFMul %float %20637 %float_0_001953125
               OpBranch %7452
       %7452 = OpLabel
      %17471 = OpPhi %float %20351 %10231 %8926 %24439
      %20892 = OpShiftRightLogical %uint %19749 %int_30
      %16230 = OpConvertUToF %float %20892
      %19268 = OpFMul %float %16230 %float_0_333333343
      %22815 = OpCompositeConstruct %v4float %19124 %19125 %17471 %19268
               OpBranch %24013
       %6619 = OpLabel
      %14900 = OpBitwiseAnd %uint %19749 %uint_1023
       %9885 = OpConvertUToF %float %14900
      %18471 = OpFMul %float %9885 %float_0_000977517106
      %22901 = OpShiftRightLogical %uint %19749 %int_10
       %7019 = OpBitwiseAnd %uint %22901 %uint_1023
      %17690 = OpConvertUToF %float %7019
      %18472 = OpFMul %float %17690 %float_0_000977517106
      %22902 = OpShiftRightLogical %uint %19749 %int_20
       %7020 = OpBitwiseAnd %uint %22902 %uint_1023
      %17691 = OpConvertUToF %float %7020
      %16818 = OpFMul %float %17691 %float_0_000977517106
      %19280 = OpShiftRightLogical %uint %19749 %int_30
      %16000 = OpConvertUToF %float %19280
      %19269 = OpFMul %float %16000 %float_0_333333343
      %22816 = OpCompositeConstruct %v4float %18471 %18472 %16818 %19269
               OpBranch %24013
       %6620 = OpLabel
      %16059 = OpBitwiseAnd %uint %19749 %uint_255
      %18498 = OpConvertUToF %float %16059
      %12026 = OpShiftRightLogical %uint %19749 %int_8
       %7384 = OpBitwiseAnd %uint %12026 %uint_255
       %7232 = OpConvertUToF %float %7384
      %12027 = OpShiftRightLogical %uint %19749 %int_16
       %7385 = OpBitwiseAnd %uint %12027 %uint_255
      %24650 = OpConvertUToF %float %7385
       %7398 = OpShiftRightLogical %uint %19749 %int_24
       %6311 = OpConvertUToF %float %7398
      %12152 = OpCompositeConstruct %v4float %18498 %7232 %24650 %6311
      %16088 = OpFMul %v4float %12152 %1203
               OpBranch %24013
      %24013 = OpLabel
      %23839 = OpPhi %v4float %16088 %6620 %22816 %6619 %22815 %7452 %9034 %7366 %9033 %16738 %19114 %21105
               OpStore %out_color %23839
               OpBranch %19578
      %19578 = OpLabel
               OpReturn
               OpFunctionEnd
#endif

const uint32_t native_edram_color_to_color_ps[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x0000629E, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0007000F, 0x00000004, 0x0000161F, 0x6E69616D, 0x00000000, 0x00000C93, 0x00000C0B, 0x00030010,
    0x0000161F, 0x00000007, 0x00030003, 0x00000002, 0x000001CC, 0x000B0004, 0x455F4C47, 0x735F5458,
    0x6C706D61, 0x656C7265, 0x745F7373, 0x75747865, 0x665F6572, 0x74636E75, 0x736E6F69, 0x00000000,
    0x000A0004, 0x475F4C47, 0x4C474F4F, 0x70635F45, 0x74735F70, 0x5F656C79, 0x656E696C, 0x7269645F,
    0x69746365, 0x00006576, 0x00080004, 0x475F4C47, 0x4C474F4F, 0x6E695F45, 0x64756C63, 0x69645F65,
    0x74636572, 0x00657669, 0x00040005, 0x0000161F, 0x6E69616D, 0x00000000, 0x00050005, 0x000004B6,
    0x736E6F43, 0x746E6174, 0x00000073, 0x00060006, 0x000004B6, 0x00000000, 0x5F747364, 0x65736162,
    0x00000000, 0x00070006, 0x000004B6, 0x00000001, 0x5F747364, 0x63746970, 0x69745F68, 0x0073656C,
    0x00060006, 0x000004B6, 0x00000002, 0x5F747364, 0x6D726F66, 0x00007461, 0x00060006, 0x000004B6,
    0x00000003, 0x5F637273, 0x65736162, 0x00000000, 0x00070006, 0x000004B6, 0x00000004, 0x5F637273,
    0x63746970, 0x69745F68, 0x0073656C, 0x00060006, 0x000004B6, 0x00000005, 0x5F637273, 0x6D726F66,
    0x00007461, 0x00060006, 0x000004B6, 0x00000006, 0x70617773, 0x6C61685F, 0x00736576, 0x00060006,
    0x000004B6, 0x00000007, 0x6E657473, 0x5F6C6963, 0x00746962, 0x00050006, 0x000004B6, 0x00000008,
    0x6C616373, 0x00007365, 0x00060006, 0x000004B6, 0x00000009, 0x656C6974, 0x6469775F, 0x00006874,
    0x00060006, 0x000004B6, 0x0000000A, 0x656C6974, 0x756F635F, 0x0000746E, 0x00030005, 0x000010B5,
    0x00000063, 0x00060005, 0x00000C93, 0x465F6C67, 0x43676172, 0x64726F6F, 0x00000000, 0x00050005,
    0x000011B3, 0x5F637273, 0x6F6C6F63, 0x00000072, 0x00050005, 0x00000C0B, 0x5F74756F, 0x6F6C6F63,
    0x00000072, 0x00030047, 0x000004B6, 0x00000002, 0x00050048, 0x000004B6, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x000004B6, 0x00000001, 0x00000023, 0x00000004, 0x00050048, 0x000004B6,
    0x00000002, 0x00000023, 0x00000008, 0x00050048, 0x000004B6, 0x00000003, 0x00000023, 0x0000000C,
    0x00050048, 0x000004B6, 0x00000004, 0x00000023, 0x00000010, 0x00050048, 0x000004B6, 0x00000005,
    0x00000023, 0x00000014, 0x00050048, 0x000004B6, 0x00000006, 0x00000023, 0x00000018, 0x00050048,
    0x000004B6, 0x00000007, 0x00000023, 0x0000001C, 0x00050048, 0x000004B6, 0x00000008, 0x00000023,
    0x00000020, 0x00050048, 0x000004B6, 0x00000009, 0x00000023, 0x00000024, 0x00050048, 0x000004B6,
    0x0000000A, 0x00000023, 0x00000028, 0x00040047, 0x00000C93, 0x0000000B, 0x0000000F, 0x00040047,
    0x000011B3, 0x00000021, 0x00000000, 0x00040047, 0x000011B3, 0x00000022, 0x00000000, 0x00040047,
    0x00000C0B, 0x0000001E, 0x00000000, 0x00020013, 0x00000008, 0x00030021, 0x00000502, 0x00000008,
    0x00030016, 0x0000000D, 0x00000020, 0x00040015, 0x0000000B, 0x00000020, 0x00000000, 0x00040017,
    0x0000001D, 0x0000000D, 0x00000004, 0x0004002B, 0x0000000D, 0x00000A0C, 0x00000000, 0x0004002B,
    0x0000000D, 0x00000696, 0x41FF0000, 0x0004002B, 0x0000000B, 0x0000016C, 0x3E800000, 0x00020014,
    0x00000009, 0x0004002B, 0x0000000B, 0x00000995, 0x007FFFFF, 0x0004002B, 0x0000000B, 0x00000998,
    0x00800000, 0x0004002B, 0x0000000B, 0x00000B81, 0x0000007D, 0x00040015, 0x0000000C, 0x00000020,
    0x00000001, 0x0004002B, 0x0000000C, 0x00000A50, 0x00000017, 0x0004002B, 0x0000000B, 0x00000A52,
    0x00000018, 0x0004002B, 0x0000000B, 0x00000105, 0xC2000000, 0x0004002B, 0x0000000B, 0x000005E4,
    0x00007FFF, 0x0004002B, 0x0000000C, 0x00000A3B, 0x00000010, 0x0004002B, 0x0000000B, 0x00000A0D,
    0x00000001, 0x0004002B, 0x0000000B, 0x00000A44, 0x000003FF, 0x0004002B, 0x0000000B, 0x00000B87,
    0x0000007F, 0x0004002B, 0x0000000C, 0x00000A20, 0x00000007, 0x0004002B, 0x0000000B, 0x00000A1F,
    0x00000007, 0x0004002B, 0x0000000B, 0x00000A0A, 0x00000000, 0x0004002B, 0x0000000D, 0x0000008A,
    0x3F800000, 0x0004002B, 0x0000000D, 0x00000BA2, 0x40400000, 0x00040017, 0x00000014, 0x0000000B,
    0x00000003, 0x00040017, 0x00000018, 0x0000000D, 0x00000003, 0x0004002B, 0x0000000D, 0x00000409,
    0x447FC000, 0x0004002B, 0x0000000C, 0x00000A29, 0x0000000A, 0x0004002B, 0x0000000B, 0x00000A10,
    0x00000002, 0x0004002B, 0x0000000C, 0x00000A47, 0x00000014, 0x0004002B, 0x0000000C, 0x00000A65,
    0x0000001E, 0x00040017, 0x00000011, 0x0000000B, 0x00000002, 0x00040017, 0x00000013, 0x0000000D,
    0x00000002, 0x0004002B, 0x0000000D, 0x00000341, 0xBF800000, 0x0004002B, 0x0000000D, 0x00000B62,
    0x46FFFE00, 0x00040017, 0x00000012, 0x0000000C, 0x00000002, 0x0004002B, 0x0000000B, 0x000001C1,
    0x0000FFFF, 0x00040017, 0x00000017, 0x0000000B, 0x00000004, 0x0004002B, 0x0000000D, 0x00000540,
    0x437F0000, 0x0004002B, 0x0000000C, 0x00000A23, 0x00000008, 0x0004002B, 0x0000000C, 0x00000A53,
    0x00000018, 0x0005002C, 0x00000013, 0x00000049, 0x00000341, 0x00000341, 0x0004002B, 0x0000000B,
    0x00000144, 0x000000FF, 0x000D001E, 0x000004B6, 0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B,
    0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B, 0x0000000B, 0x00040020,
    0x00000733, 0x00000009, 0x000004B6, 0x0004003B, 0x00000733, 0x000010B5, 0x00000009, 0x00040020,
    0x00000288, 0x00000009, 0x0000000B, 0x00040020, 0x0000029A, 0x00000001, 0x0000001D, 0x0004003B,
    0x0000029A, 0x00000C93, 0x00000001, 0x0004002B, 0x0000000C, 0x00000A26, 0x00000009, 0x0004002B,
    0x0000000B, 0x00000A3A, 0x00000010, 0x0004002B, 0x0000000B, 0x00000A16, 0x00000004, 0x0004002B,
    0x0000000C, 0x00000A0B, 0x00000000, 0x0004002B, 0x0000000C, 0x00000A0E, 0x00000001, 0x0004002B,
    0x0000000C, 0x00000A1D, 0x00000006, 0x0004002B, 0x0000000C, 0x00000A14, 0x00000003, 0x0004002B,
    0x0000000C, 0x00000A17, 0x00000004, 0x00090019, 0x00000096, 0x0000000D, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x00000313, 0x00000000, 0x00000096,
    0x0004003B, 0x00000313, 0x000011B3, 0x00000000, 0x0004002B, 0x0000000B, 0x00000A82, 0x00000028,
    0x00040020, 0x0000029B, 0x00000003, 0x0000001D, 0x0004003B, 0x0000029B, 0x00000C0B, 0x00000003,
    0x0004002B, 0x0000000C, 0x00000A1A, 0x00000005, 0x0004002B, 0x0000000C, 0x00000A11, 0x00000002,
    0x0005002C, 0x00000011, 0x00000763, 0x00000A16, 0x00000A16, 0x0005002C, 0x00000011, 0x00000739,
    0x00000A10, 0x00000A10, 0x0005002C, 0x00000011, 0x00000724, 0x00000A0D, 0x00000A0D, 0x0007002C,
    0x0000001D, 0x00000B7A, 0x00000A0C, 0x00000A0C, 0x00000A0C, 0x00000A0C, 0x0007002C, 0x0000001D,
    0x00000504, 0x0000008A, 0x0000008A, 0x0000008A, 0x0000008A, 0x0006002C, 0x00000018, 0x00000A2C,
    0x00000A0C, 0x00000A0C, 0x00000A0C, 0x0006002C, 0x00000018, 0x00000A18, 0x0000008A, 0x0000008A,
    0x0000008A, 0x0005002C, 0x00000013, 0x00000300, 0x0000008A, 0x0000008A, 0x0005002C, 0x00000011,
    0x000007DF, 0x000001C1, 0x000001C1, 0x0005002C, 0x00000012, 0x00000867, 0x00000A3B, 0x00000A3B,
    0x0004002B, 0x0000000D, 0x00000A38, 0x38000100, 0x0005002C, 0x00000013, 0x00000685, 0x00000A38,
    0x00000A38, 0x0004002B, 0x0000000D, 0x000003A8, 0x3C000000, 0x0004002B, 0x0000000D, 0x0000048C,
    0x3B000000, 0x0004002B, 0x0000000D, 0x00000149, 0x3EAAAAAB, 0x0004002B, 0x0000000D, 0x000006FE,
    0x3A802008, 0x0004002B, 0x0000000D, 0x0000017A, 0x3B808081, 0x0007002C, 0x0000001D, 0x000004B3,
    0x0000017A, 0x0000017A, 0x0000017A, 0x0000017A, 0x00050036, 0x00000008, 0x0000161F, 0x00000000,
    0x00000502, 0x000200F8, 0x00003B06, 0x000300F7, 0x00004C7A, 0x00000000, 0x000300FB, 0x00000A0A,
    0x00002E68, 0x000200F8, 0x00002E68, 0x00050041, 0x00000288, 0x000060D7, 0x000010B5, 0x00000A23,
    0x0004003D, 0x0000000B, 0x000036DB, 0x000060D7, 0x000500C7, 0x0000000B, 0x00004613, 0x000036DB,
    0x00000144, 0x000500C2, 0x0000000B, 0x00005844, 0x000036DB, 0x00000A23, 0x0004003D, 0x0000001D,
    0x00004C86, 0x00000C93, 0x0007004F, 0x00000013, 0x0000249C, 0x00004C86, 0x00004C86, 0x00000000,
    0x00000001, 0x0004006D, 0x00000011, 0x00001A7A, 0x0000249C, 0x00050041, 0x00000288, 0x00004892,
    0x000010B5, 0x00000A26, 0x0004003D, 0x0000000B, 0x000036A3, 0x00004892, 0x00050050, 0x00000011,
    0x000025C7, 0x000036A3, 0x00000A3A, 0x00050050, 0x00000011, 0x00005E4D, 0x00004613, 0x00004613,
    0x00050084, 0x00000011, 0x00004076, 0x000025C7, 0x00005E4D, 0x00050086, 0x00000011, 0x000027C9,
    0x00004076, 0x00000763, 0x00050086, 0x00000011, 0x000060A8, 0x00001A7A, 0x000027C9, 0x00050041,
    0x00000288, 0x00005088, 0x000010B5, 0x00000A0B, 0x0004003D, 0x0000000B, 0x00002B54, 0x00005088,
    0x00050051, 0x0000000B, 0x00005523, 0x000060A8, 0x00000001, 0x00050041, 0x00000288, 0x00001940,
    0x000010B5, 0x00000A0E, 0x0004003D, 0x0000000B, 0x00003BCA, 0x00001940, 0x00050084, 0x0000000B,
    0x0000198A, 0x00005523, 0x00003BCA, 0x00050080, 0x0000000B, 0x00001ACA, 0x00002B54, 0x0000198A,
    0x00050051, 0x0000000B, 0x00005D75, 0x000060A8, 0x00000000, 0x00050080, 0x0000000B, 0x00002255,
    0x00001ACA, 0x00005D75, 0x00050041, 0x00000288, 0x0000263A, 0x000010B5, 0x00000A29, 0x0004003D,
    0x0000000B, 0x00005B46, 0x0000263A, 0x00050089, 0x0000000B, 0x000059F1, 0x00002255, 0x00005B46,
    0x00050084, 0x00000011, 0x00003780, 0x000060A8, 0x000027C9, 0x00050082, 0x00000011, 0x00004BE6,
    0x00001A7A, 0x00003780, 0x00050050, 0x00000011, 0x00001A37, 0x00005844, 0x00005844, 0x00050084,
    0x00000011, 0x0000629D, 0x000025C7, 0x00001A37, 0x00050086, 0x00000011, 0x00001C4A, 0x0000629D,
    0x00000763, 0x000500AB, 0x00000009, 0x00002CC2, 0x00005844, 0x00004613, 0x000300F7, 0x00005490,
    0x00000000, 0x000400FA, 0x00002CC2, 0x00005192, 0x00005490, 0x000200F8, 0x00005192, 0x00050084,
    0x00000011, 0x00002EF4, 0x00004BE6, 0x00000739, 0x00050080, 0x00000011, 0x000059E7, 0x00002EF4,
    0x00000724, 0x00050084, 0x00000011, 0x0000495C, 0x000059E7, 0x00001A37, 0x00050084, 0x0000000B,
    0x00006004, 0x00004613, 0x00000A10, 0x00050050, 0x00000011, 0x00001F74, 0x00006004, 0x00006004,
    0x00050086, 0x00000011, 0x00002B67, 0x0000495C, 0x00001F74, 0x000200F9, 0x00005490, 0x000200F8,
    0x00005490, 0x000700F5, 0x00000011, 0x00004D24, 0x00004BE6, 0x00002E68, 0x00002B67, 0x00005192,
    0x00050041, 0x00000288, 0x00001A58, 0x000010B5, 0x00000A1D, 0x0004003D, 0x0000000B, 0x000050DF,
    0x00001A58, 0x000500AB, 0x00000009, 0x00003271, 0x000050DF, 0x00000A0A, 0x000300F7, 0x00004CC1,
    0x00000000, 0x000400FA, 0x00003271, 0x0000260D, 0x00004CC1, 0x000200F8, 0x0000260D, 0x00050051,
    0x0000000B, 0x00004C9A, 0x00004D24, 0x00000000, 0x00050051, 0x0000000B, 0x000029C4, 0x00001C4A,
    0x00000000, 0x000500C2, 0x0000000B, 0x00005575, 0x000029C4, 0x00000A0E, 0x00050080, 0x0000000B,
    0x00001FA6, 0x00004C9A, 0x00005575, 0x00050089, 0x0000000B, 0x00004E28, 0x00001FA6, 0x000029C4,
    0x00060052, 0x00000011, 0x00004675, 0x00004E28, 0x00004D24, 0x00000000, 0x000200F9, 0x00004CC1,
    0x000200F8, 0x00004CC1, 0x000700F5, 0x00000011, 0x000022DD, 0x00004D24, 0x00005490, 0x00004675,
    0x0000260D, 0x00050080, 0x0000000B, 0x0000551F, 0x000059F1, 0x00005B46, 0x00050041, 0x00000288,
    0x00003317, 0x000010B5, 0x00000A14, 0x0004003D, 0x0000000B, 0x00006060, 0x00003317, 0x00050082,
    0x0000000B, 0x00005247, 0x0000551F, 0x00006060, 0x00050089, 0x0000000B, 0x00002C86, 0x00005247,
    0x00005B46, 0x00050041, 0x00000288, 0x0000452B, 0x000010B5, 0x00000A17, 0x0004003D, 0x0000000B,
    0x000020AB, 0x0000452B, 0x00050089, 0x0000000B, 0x000054E1, 0x00002C86, 0x000020AB, 0x00050086,
    0x0000000B, 0x00001D24, 0x00002C86, 0x000020AB, 0x00050050, 0x00000011, 0x00004A4B, 0x000054E1,
    0x00001D24, 0x00050084, 0x00000011, 0x00002549, 0x00004A4B, 0x00001C4A, 0x00050080, 0x00000011,
    0x00005947, 0x00002549, 0x000022DD, 0x0004007C, 0x00000012, 0x00005080, 0x00005947, 0x0004003D,
    0x00000096, 0x0000232E, 0x000011B3, 0x0007005F, 0x0000001D, 0x000030D5, 0x0000232E, 0x00005080,
    0x00000002, 0x00000A0B, 0x000500AA, 0x00000009, 0x000053B4, 0x000036A3, 0x00000A82, 0x000300F7,
    0x00001FDE, 0x00000000, 0x000400FA, 0x000053B4, 0x00003F5C, 0x00001FDE, 0x000200F8, 0x00003F5C,
    0x0003003E, 0x00000C0B, 0x000030D5, 0x000200F9, 0x00004C7A, 0x000200F8, 0x00001FDE, 0x00050041,
    0x00000288, 0x00005E1A, 0x000010B5, 0x00000A1A, 0x0004003D, 0x0000000B, 0x00001AA6, 0x00005E1A,
    0x000300F7, 0x00002C98, 0x00000000, 0x001100FB, 0x00001AA6, 0x00003167, 0x00000002, 0x00004E83,
    0x0000000A, 0x00004E83, 0x00000003, 0x00004D29, 0x0000000C, 0x00004D29, 0x00000004, 0x000048E9,
    0x00000006, 0x000025C3, 0x0000000E, 0x00002845, 0x000200F8, 0x00002845, 0x00050051, 0x0000000D,
    0x00002A2A, 0x000030D5, 0x00000000, 0x0004007C, 0x0000000B, 0x000034C3, 0x00002A2A, 0x000200F9,
    0x00002C98, 0x000200F8, 0x000025C3, 0x0007004F, 0x00000013, 0x00005CDB, 0x000030D5, 0x000030D5,
    0x00000000, 0x00000001, 0x0006000C, 0x0000000B, 0x00002B38, 0x00000001, 0x0000003A, 0x00005CDB,
    0x000200F9, 0x00002C98, 0x000200F8, 0x000048E9, 0x0007004F, 0x00000013, 0x0000227A, 0x000030D5,
    0x000030D5, 0x00000000, 0x00000001, 0x0008000C, 0x00000013, 0x00005403, 0x00000001, 0x0000002B,
    0x0000227A, 0x00000049, 0x00000300, 0x0005008E, 0x00000013, 0x00005571, 0x00005403, 0x00000B62,
    0x0006000C, 0x00000013, 0x000045F8, 0x00000001, 0x00000001, 0x00005571, 0x0004006E, 0x00000012,
    0x00005444, 0x000045F8, 0x0004007C, 0x00000011, 0x00002B45, 0x00005444, 0x000500C7, 0x00000011,
    0x00003BC7, 0x00002B45, 0x000007DF, 0x00050051, 0x0000000B, 0x000022D2, 0x00003BC7, 0x00000000,
    0x00050051, 0x0000000B, 0x00001DE4, 0x00003BC7, 0x00000001, 0x000500C4, 0x0000000B, 0x000039A1,
    0x00001DE4, 0x00000A3B, 0x000500C5, 0x0000000B, 0x00002A39, 0x000022D2, 0x000039A1, 0x000200F9,
    0x00002C98, 0x000200F8, 0x00004D29, 0x00050051, 0x0000000D, 0x00002BA1, 0x000030D5, 0x00000000,
    0x0008000C, 0x0000000D, 0x00001FE7, 0x00000001, 0x0000002B, 0x00002BA1, 0x00000A0C, 0x00000696,
    0x0004007C, 0x0000000B, 0x00004D15, 0x00001FE7, 0x000500B0, 0x00000009, 0x000045D2, 0x00004D15,
    0x0000016C, 0x000300F7, 0x0000346E, 0x00000000, 0x000400FA, 0x000045D2, 0x00003945, 0x00005D41,
    0x000200F8, 0x00005D41, 0x00050080, 0x0000000B, 0x00001B50, 0x00004D15, 0x00000105, 0x000200F9,
    0x0000346E, 0x000200F8, 0x00003945, 0x000500C7, 0x0000000B, 0x00004FC9, 0x00004D15, 0x00000995,
    0x000500C5, 0x0000000B, 0x00004DF0, 0x00004FC9, 0x00000998, 0x000500C2, 0x0000000B, 0x00002DE6,
    0x00004D15, 0x00000A50, 0x00050082, 0x0000000B, 0x00002F5A, 0x00000B81, 0x00002DE6, 0x0007000C,
    0x0000000B, 0x00003F35, 0x00000001, 0x00000026, 0x00002F5A, 0x00000A52, 0x000500C2, 0x0000000B,
    0x00006299, 0x00004DF0, 0x00003F35, 0x000200F9, 0x0000346E, 0x000200F8, 0x0000346E, 0x000700F5,
    0x0000000B, 0x00002C70, 0x00006299, 0x00003945, 0x00001B50, 0x00005D41, 0x00050080, 0x0000000B,
    0x000052AF, 0x00002C70, 0x000005E4, 0x000500C2, 0x0000000B, 0x0000323C, 0x00002C70, 0x00000A3B,
    0x000500C7, 0x0000000B, 0x00005C7D, 0x0000323C, 0x00000A0D, 0x00050080, 0x0000000B, 0x000025A9,
    0x000052AF, 0x00005C7D, 0x000500C2, 0x0000000B, 0x00004D37, 0x000025A9, 0x00000A3B, 0x000500C7,
    0x0000000B, 0x0000246A, 0x00004D37, 0x00000A44, 0x00050051, 0x0000000D, 0x00002435, 0x000030D5,
    0x00000001, 0x0008000C, 0x0000000D, 0x00004188, 0x00000001, 0x0000002B, 0x00002435, 0x00000A0C,
    0x00000696, 0x0004007C, 0x0000000B, 0x00004D16, 0x00004188, 0x000500B0, 0x00000009, 0x000045D3,
    0x00004D16, 0x0000016C, 0x000300F7, 0x0000346F, 0x00000000, 0x000400FA, 0x000045D3, 0x00003946,
    0x00005D42, 0x000200F8, 0x00005D42, 0x00050080, 0x0000000B, 0x00001B51, 0x00004D16, 0x00000105,
    0x000200F9, 0x0000346F, 0x000200F8, 0x00003946, 0x000500C7, 0x0000000B, 0x00004FCA, 0x00004D16,
    0x00000995, 0x000500C5, 0x0000000B, 0x00004DF1, 0x00004FCA, 0x00000998, 0x000500C2, 0x0000000B,
    0x00002DE7, 0x00004D16, 0x00000A50, 0x00050082, 0x0000000B, 0x00002F5B, 0x00000B81, 0x00002DE7,
    0x0007000C, 0x0000000B, 0x00003F36, 0x00000001, 0x00000026, 0x00002F5B, 0x00000A52, 0x000500C2,
    0x0000000B, 0x0000629A, 0x00004DF1, 0x00003F36, 0x000200F9, 0x0000346F, 0x000200F8, 0x0000346F,
    0x000700F5, 0x0000000B, 0x00002C71, 0x0000629A, 0x00003946, 0x00001B51, 0x00005D42, 0x00050080,
    0x0000000B, 0x000052B0, 0x00002C71, 0x000005E4, 0x000500C2, 0x0000000B, 0x0000323D, 0x00002C71,
    0x00000A3B, 0x000500C7, 0x0000000B, 0x00005C7E, 0x0000323D, 0x00000A0D, 0x00050080, 0x0000000B,
    0x000025AA, 0x000052B0, 0x00005C7E, 0x000500C2, 0x0000000B, 0x000055C0, 0x000025AA, 0x00000A3B,
    0x000500C7, 0x0000000B, 0x00002EC2, 0x000055C0, 0x00000A44, 0x000500C4, 0x0000000B, 0x0000557E,
    0x00002EC2, 0x00000A29, 0x000500C5, 0x0000000B, 0x00003A7D, 0x0000246A, 0x0000557E, 0x00050051,
    0x0000000D, 0x00002C74, 0x000030D5, 0x00000002, 0x0008000C, 0x0000000D, 0x00003B3B, 0x00000001,
    0x0000002B, 0x00002C74, 0x00000A0C, 0x00000696, 0x0004007C, 0x0000000B, 0x00004D17, 0x00003B3B,
    0x000500B0, 0x00000009, 0x000045D4, 0x00004D17, 0x0000016C, 0x000300F7, 0x00003470, 0x00000000,
    0x000400FA, 0x000045D4, 0x00003947, 0x00005D43, 0x000200F8, 0x00005D43, 0x00050080, 0x0000000B,
    0x00001B52, 0x00004D17, 0x00000105, 0x000200F9, 0x00003470, 0x000200F8, 0x00003947, 0x000500C7,
    0x0000000B, 0x00004FCB, 0x00004D17, 0x00000995, 0x000500C5, 0x0000000B, 0x00004DF2, 0x00004FCB,
    0x00000998, 0x000500C2, 0x0000000B, 0x00002DE8, 0x00004D17, 0x00000A50, 0x00050082, 0x0000000B,
    0x00002F5C, 0x00000B81, 0x00002DE8, 0x0007000C, 0x0000000B, 0x00003F37, 0x00000001, 0x00000026,
    0x00002F5C, 0x00000A52, 0x000500C2, 0x0000000B, 0x0000629B, 0x00004DF2, 0x00003F37, 0x000200F9,
    0x00003470, 0x000200F8, 0x00003470, 0x000700F5, 0x0000000B, 0x00002C72, 0x0000629B, 0x00003947,
    0x00001B52, 0x00005D43, 0x00050080, 0x0000000B, 0x000052B1, 0x00002C72, 0x000005E4, 0x000500C2,
    0x0000000B, 0x0000323E, 0x00002C72, 0x00000A3B, 0x000500C7, 0x0000000B, 0x00005C7F, 0x0000323E,
    0x00000A0D, 0x00050080, 0x0000000B, 0x000025AB, 0x000052B1, 0x00005C7F, 0x000500C2, 0x0000000B,
    0x000055C1, 0x000025AB, 0x00000A3B, 0x000500C7, 0x0000000B, 0x00002EC3, 0x000055C1, 0x00000A44,
    0x000500C4, 0x0000000B, 0x0000557F, 0x00002EC3, 0x00000A47, 0x000500C5, 0x0000000B, 0x00003A7E,
    0x00003A7D, 0x0000557F, 0x00050051, 0x0000000D, 0x00002D1F, 0x000030D5, 0x00000003, 0x0008000C,
    0x0000000D, 0x000028FB, 0x00000001, 0x0000002B, 0x00002D1F, 0x00000A0C, 0x0000008A, 0x00050085,
    0x0000000D, 0x000051B8, 0x000028FB, 0x00000BA2, 0x0006000C, 0x0000000D, 0x00003CB1, 0x00000001,
    0x00000001, 0x000051B8, 0x0004006D, 0x0000000B, 0x00004CF2, 0x00003CB1, 0x000500C4, 0x0000000B,
    0x00003F29, 0x00004CF2, 0x00000A65, 0x000500C5, 0x0000000B, 0x000036C4, 0x00003A7E, 0x00003F29,
    0x000200F9, 0x00002C98, 0x000200F8, 0x00004E83, 0x0008004F, 0x00000018, 0x0000227B, 0x000030D5,
    0x000030D5, 0x00000000, 0x00000001, 0x00000002, 0x0008000C, 0x00000018, 0x00005404, 0x00000001,
    0x0000002B, 0x0000227B, 0x00000A2C, 0x00000A18, 0x0005008E, 0x00000018, 0x0000555E, 0x00005404,
    0x00000409, 0x0006000C, 0x00000018, 0x00004374, 0x00000001, 0x00000001, 0x0000555E, 0x0004006D,
    0x00000014, 0x00003E13, 0x00004374, 0x00050051, 0x0000000B, 0x00002BFD, 0x00003E13, 0x00000000,
    0x00050051, 0x0000000B, 0x00002FDB, 0x00003E13, 0x00000001, 0x000500C4, 0x0000000B, 0x00002D29,
    0x00002FDB, 0x00000A29, 0x000500C5, 0x0000000B, 0x00004D66, 0x00002BFD, 0x00002D29, 0x00050051,
    0x0000000B, 0x000053E4, 0x00003E13, 0x00000002, 0x000500C4, 0x0000000B, 0x00002170, 0x000053E4,
    0x00000A47, 0x000500C5, 0x0000000B, 0x00003FE9, 0x00004D66, 0x00002170, 0x00050051, 0x0000000D,
    0x00002D20, 0x000030D5, 0x00000003, 0x0008000C, 0x0000000D, 0x000028FC, 0x00000001, 0x0000002B,
    0x00002D20, 0x00000A0C, 0x0000008A, 0x00050085, 0x0000000D, 0x000051B9, 0x000028FC, 0x00000BA2,
    0x0006000C, 0x0000000D, 0x00003CB2, 0x00000001, 0x00000001, 0x000051B9, 0x0004006D, 0x0000000B,
    0x00004CF3, 0x00003CB2, 0x000500C4, 0x0000000B, 0x00003F2A, 0x00004CF3, 0x00000A65, 0x000500C5,
    0x0000000B, 0x000036C5, 0x00003FE9, 0x00003F2A, 0x000200F9, 0x00002C98, 0x000200F8, 0x00003167,
    0x0008000C, 0x0000001D, 0x00005E60, 0x00000001, 0x0000002B, 0x000030D5, 0x00000B7A, 0x00000504,
    0x0005008E, 0x0000001D, 0x0000349E, 0x00005E60, 0x00000540, 0x0006000C, 0x0000001D, 0x00004375,
    0x00000001, 0x00000001, 0x0000349E, 0x0004006D, 0x00000017, 0x00003E14, 0x00004375, 0x00050051,
    0x0000000B, 0x00002BFE, 0x00003E14, 0x00000000, 0x00050051, 0x0000000B, 0x00002FDC, 0x00003E14,
    0x00000001, 0x000500C4, 0x0000000B, 0x00002D2A, 0x00002FDC, 0x00000A23, 0x000500C5, 0x0000000B,
    0x00004D67, 0x00002BFE, 0x00002D2A, 0x00050051, 0x0000000B, 0x000053E5, 0x00003E14, 0x00000002,
    0x000500C4, 0x0000000B, 0x00002171, 0x000053E5, 0x00000A3B, 0x000500C5, 0x0000000B, 0x00004D68,
    0x00004D67, 0x00002171, 0x00050051, 0x0000000B, 0x000053E6, 0x00003E14, 0x00000003, 0x000500C4,
    0x0000000B, 0x00002DE9, 0x000053E6, 0x00000A53, 0x000500C5, 0x0000000B, 0x00002A3A, 0x00004D68,
    0x00002DE9, 0x000200F9, 0x00002C98, 0x000200F8, 0x00002C98, 0x000F00F5, 0x0000000B, 0x00004D25,
    0x00002A3A, 0x00003167, 0x000036C5, 0x00004E83, 0x000036C4, 0x00003470, 0x00002A39, 0x000048E9,
    0x00002B38, 0x000025C3, 0x000034C3, 0x00002845, 0x00050041, 0x00000288, 0x00001FFC, 0x000010B5,
    0x00000A11, 0x0004003D, 0x0000000B, 0x00001DCF, 0x00001FFC, 0x000300F7, 0x00005DCD, 0x00000000,
    0x001100FB, 0x00001DCF, 0x000019DC, 0x00000002, 0x000019DB, 0x0000000A, 0x000019DB, 0x00000003,
    0x00002050, 0x0000000C, 0x00002050, 0x00000004, 0x00001CC6, 0x00000006, 0x00004162, 0x0000000E,
    0x00005271, 0x000200F8, 0x00005271, 0x0004007C, 0x0000000D, 0x000024A9, 0x00004D25, 0x00070050,
    0x0000001D, 0x00004AAA, 0x000024A9, 0x00000A0C, 0x00000A0C, 0x00000A0C, 0x000200F9, 0x00005DCD,
    0x000200F8, 0x00004162, 0x0006000C, 0x00000013, 0x00002237, 0x00000001, 0x0000003E, 0x00004D25,
    0x00050051, 0x0000000D, 0x0000319B, 0x00002237, 0x00000000, 0x00050051, 0x0000000D, 0x000050BE,
    0x00002237, 0x00000001, 0x00070050, 0x0000001D, 0x00002349, 0x0000319B, 0x000050BE, 0x00000A0C,
    0x00000A0C, 0x000200F9, 0x00005DCD, 0x000200F8, 0x00001CC6, 0x000500C4, 0x0000000B, 0x000046F1,
    0x00004D25, 0x00000A3B, 0x0004007C, 0x0000000C, 0x00001B96, 0x000046F1, 0x0004007C, 0x0000000C,
    0x00004FFD, 0x00004D25, 0x00050050, 0x00000012, 0x00005782, 0x00001B96, 0x00004FFD, 0x000500C3,
    0x00000012, 0x00005795, 0x00005782, 0x00000867, 0x0004006F, 0x00000013, 0x00003C65, 0x00005795,
    0x00050085, 0x00000013, 0x00003D54, 0x00003C65, 0x00000685, 0x0007000C, 0x00000013, 0x00004EBA,
    0x00000001, 0x00000028, 0x00003D54, 0x00000049, 0x00050051, 0x0000000D, 0x00004F4C, 0x00004EBA,
    0x00000000, 0x00050051, 0x0000000D, 0x0000494C, 0x00004EBA, 0x00000001, 0x00070050, 0x0000001D,
    0x0000234A, 0x00004F4C, 0x0000494C, 0x00000A0C, 0x00000A0C, 0x000200F9, 0x00005DCD, 0x000200F8,
    0x00002050, 0x000500C7, 0x0000000B, 0x00004E6F, 0x00004D25, 0x00000A44, 0x000500C7, 0x0000000B,
    0x00005D8C, 0x00004D25, 0x00000B87, 0x000500C2, 0x0000000B, 0x00005366, 0x00004E6F, 0x00000A20,
    0x000500C7, 0x0000000B, 0x00003546, 0x00005366, 0x00000A1F, 0x000500AA, 0x00000009, 0x00004CE4,
    0x00003546, 0x00000A0A, 0x000300F7, 0x00001D1A, 0x00000000, 0x000400FA, 0x00004CE4, 0x000027F5,
    0x00005F75, 0x000200F8, 0x00005F75, 0x00040070, 0x0000000D, 0x00005776, 0x00005D8C, 0x00050085,
    0x0000000D, 0x00004BFE, 0x00005776, 0x000003A8, 0x00050081, 0x0000000D, 0x00004587, 0x0000008A,
    0x00004BFE, 0x00040070, 0x0000000D, 0x00003581, 0x00003546, 0x00050083, 0x0000000D, 0x000045CD,
    0x00003581, 0x00000BA2, 0x0006000C, 0x0000000D, 0x0000583F, 0x00000001, 0x0000001D, 0x000045CD,
    0x00050085, 0x0000000D, 0x000022DB, 0x00004587, 0x0000583F, 0x000200F9, 0x00001D1A, 0x000200F8,
    0x000027F5, 0x00040070, 0x0000000D, 0x0000509B, 0x00005D8C, 0x00050085, 0x0000000D, 0x00004F7D,
    0x0000509B, 0x0000048C, 0x000200F9, 0x00001D1A, 0x000200F8, 0x00001D1A, 0x000700F5, 0x0000000D,
    0x00004AB4, 0x00004F7D, 0x000027F5, 0x000022DB, 0x00005F75, 0x000500C2, 0x0000000B, 0x00001BB7,
    0x00004D25, 0x00000A29, 0x000500C7, 0x0000000B, 0x0000308C, 0x00001BB7, 0x00000A44, 0x000500C7,
    0x0000000B, 0x00004AC3, 0x00001BB7, 0x00000B87, 0x000500C2, 0x0000000B, 0x00005367, 0x0000308C,
    0x00000A20, 0x000500C7, 0x0000000B, 0x00003547, 0x00005367, 0x00000A1F, 0x000500AA, 0x00000009,
    0x00004CE5, 0x00003547, 0x00000A0A, 0x000300F7, 0x00001D1B, 0x00000000, 0x000400FA, 0x00004CE5,
    0x000027F6, 0x00005F76, 0x000200F8, 0x00005F76, 0x00040070, 0x0000000D, 0x00005777, 0x00004AC3,
    0x00050085, 0x0000000D, 0x00004BFF, 0x00005777, 0x000003A8, 0x00050081, 0x0000000D, 0x00004588,
    0x0000008A, 0x00004BFF, 0x00040070, 0x0000000D, 0x00003582, 0x00003547, 0x00050083, 0x0000000D,
    0x000045CE, 0x00003582, 0x00000BA2, 0x0006000C, 0x0000000D, 0x00005840, 0x00000001, 0x0000001D,
    0x000045CE, 0x00050085, 0x0000000D, 0x000022DC, 0x00004588, 0x00005840, 0x000200F9, 0x00001D1B,
    0x000200F8, 0x000027F6, 0x00040070, 0x0000000D, 0x0000509C, 0x00004AC3, 0x00050085, 0x0000000D,
    0x00004F7E, 0x0000509C, 0x0000048C, 0x000200F9, 0x00001D1B, 0x000200F8, 0x00001D1B, 0x000700F5,
    0x0000000D, 0x00004AB5, 0x00004F7E, 0x000027F6, 0x000022DC, 0x00005F76, 0x000500C2, 0x0000000B,
    0x00001BB8, 0x00004D25, 0x00000A47, 0x000500C7, 0x0000000B, 0x0000308D, 0x00001BB8, 0x00000A44,
    0x000500C7, 0x0000000B, 0x00004AC4, 0x00001BB8, 0x00000B87, 0x000500C2, 0x0000000B, 0x00005368,
    0x0000308D, 0x00000A20, 0x000500C7, 0x0000000B, 0x00003548, 0x00005368, 0x00000A1F, 0x000500AA,
    0x00000009, 0x00004CE6, 0x00003548, 0x00000A0A, 0x000300F7, 0x00001D1C, 0x00000000, 0x000400FA,
    0x00004CE6, 0x000027F7, 0x00005F77, 0x000200F8, 0x00005F77, 0x00040070, 0x0000000D, 0x00005778,
    0x00004AC4, 0x00050085, 0x0000000D, 0x00004C00, 0x00005778, 0x000003A8, 0x00050081, 0x0000000D,
    0x00004589, 0x0000008A, 0x00004C00, 0x00040070, 0x0000000D, 0x00003583, 0x00003548, 0x00050083,
    0x0000000D, 0x000045CF, 0x00003583, 0x00000BA2, 0x0006000C, 0x0000000D, 0x00005841, 0x00000001,
    0x0000001D, 0x000045CF, 0x00050085, 0x0000000D, 0x000022DE, 0x00004589, 0x00005841, 0x000200F9,
    0x00001D1C, 0x000200F8, 0x000027F7, 0x00040070, 0x0000000D, 0x0000509D, 0x00004AC4, 0x00050085,
    0x0000000D, 0x00004F7F, 0x0000509D, 0x0000048C, 0x000200F9, 0x00001D1C, 0x000200F8, 0x00001D1C,
    0x000700F5, 0x0000000D, 0x0000443F, 0x00004F7F, 0x000027F7, 0x000022DE, 0x00005F77, 0x000500C2,
    0x0000000B, 0x0000519C, 0x00004D25, 0x00000A65, 0x00040070, 0x0000000D, 0x00003F66, 0x0000519C,
    0x00050085, 0x0000000D, 0x00004B44, 0x00003F66, 0x00000149, 0x00070050, 0x0000001D, 0x0000591F,
    0x00004AB4, 0x00004AB5, 0x0000443F, 0x00004B44, 0x000200F9, 0x00005DCD, 0x000200F8, 0x000019DB,
    0x000500C7, 0x0000000B, 0x00003A34, 0x00004D25, 0x00000A44, 0x00040070, 0x0000000D, 0x0000269D,
    0x00003A34, 0x00050085, 0x0000000D, 0x00004827, 0x0000269D, 0x000006FE, 0x000500C2, 0x0000000B,
    0x00005975, 0x00004D25, 0x00000A29, 0x000500C7, 0x0000000B, 0x00001B6B, 0x00005975, 0x00000A44,
    0x00040070, 0x0000000D, 0x0000451A, 0x00001B6B, 0x00050085, 0x0000000D, 0x00004828, 0x0000451A,
    0x000006FE, 0x000500C2, 0x0000000B, 0x00005976, 0x00004D25, 0x00000A47, 0x000500C7, 0x0000000B,
    0x00001B6C, 0x00005976, 0x00000A44, 0x00040070, 0x0000000D, 0x0000451B, 0x00001B6C, 0x00050085,
    0x0000000D, 0x000041B2, 0x0000451B, 0x000006FE, 0x000500C2, 0x0000000B, 0x00004B50, 0x00004D25,
    0x00000A65, 0x00040070, 0x0000000D, 0x00003E80, 0x00004B50, 0x00050085, 0x0000000D, 0x00004B45,
    0x00003E80, 0x00000149, 0x00070050, 0x0000001D, 0x00005920, 0x00004827, 0x00004828, 0x000041B2,
    0x00004B45, 0x000200F9, 0x00005DCD, 0x000200F8, 0x000019DC, 0x000500C7, 0x0000000B, 0x00003EBB,
    0x00004D25, 0x00000144, 0x00040070, 0x0000000D, 0x00004842, 0x00003EBB, 0x000500C2, 0x0000000B,
    0x00002EFA, 0x00004D25, 0x00000A23, 0x000500C7, 0x0000000B, 0x00001CD8, 0x00002EFA, 0x00000144,
    0x00040070, 0x0000000D, 0x00001C40, 0x00001CD8, 0x000500C2, 0x0000000B, 0x00002EFB, 0x00004D25,
    0x00000A3B, 0x000500C7, 0x0000000B, 0x00001CD9, 0x00002EFB, 0x00000144, 0x00040070, 0x0000000D,
    0x0000604A, 0x00001CD9, 0x000500C2, 0x0000000B, 0x00001CE6, 0x00004D25, 0x00000A53, 0x00040070,
    0x0000000D, 0x000018A7, 0x00001CE6, 0x00070050, 0x0000001D, 0x00002F78, 0x00004842, 0x00001C40,
    0x0000604A, 0x000018A7, 0x00050085, 0x0000001D, 0x00003ED8, 0x00002F78, 0x000004B3, 0x000200F9,
    0x00005DCD, 0x000200F8, 0x00005DCD, 0x000F00F5, 0x0000001D, 0x00005D1F, 0x00003ED8, 0x000019DC,
    0x00005920, 0x000019DB, 0x0000591F, 0x00001D1C, 0x0000234A, 0x00001CC6, 0x00002349, 0x00004162,
    0x00004AAA, 0x00005271, 0x0003003E, 0x00000C0B, 0x00005D1F, 0x000200F9, 0x00004C7A, 0x000200F8,
    0x00004C7A, 0x000100FD, 0x00010038,
};
