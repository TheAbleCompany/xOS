MESA := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

BUILD := $(MESA)/build
OBJ   := $(MESA)/obj

HOST_CC  := gcc
HOST_CXX := g++
HOST_LD  := ld

PY ?= python3

COMMON_DEFS := -DHAVE_PTHREAD=1 -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 \
	-DHAVE_FUNC_ATTRIBUTE_CONST=1 -DHAVE_FUNC_ATTRIBUTE_FLATTEN=1 \
	-DHAVE_FUNC_ATTRIBUTE_MALLOC=1 -DHAVE_FUNC_ATTRIBUTE_PURE=1 \
	-DHAVE_FUNC_ATTRIBUTE_UNUSED=1 -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT=1 \
	-DHAVE_FUNC_ATTRIBUTE_WEAK=1 -DHAVE_FUNC_ATTRIBUTE_FORMAT=1 \
	-DHAVE_FUNC_ATTRIBUTE_PACKED=1 -DHAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL=1 \
	-DHAVE_FUNC_ATTRIBUTE_ALIAS=1 -DHAVE_FUNC_ATTRIBUTE_NORETURN=1 \
	-DHAVE_FUNC_ATTRIBUTE_OPTIMIZE=1 -DHAVE_FUNC_ATTRIBUTE_VISIBILITY=1 \
	-DHAVE___BUILTIN_EXPECT=1 -DHAVE___BUILTIN_CLZ=1 -DHAVE___BUILTIN_CLZLL=1 \
	-DHAVE___BUILTIN_CTZ=1 -DHAVE___BUILTIN_FFS=1 -DHAVE___BUILTIN_FFSLL=1 \
	-DHAVE___BUILTIN_POPCOUNT=1 -DHAVE___BUILTIN_POPCOUNTLL=1 \
	-DHAVE___BUILTIN_BSWAP32=1 -DHAVE___BUILTIN_BSWAP64=1 \
	-DHAVE___BUILTIN_UNREACHABLE=1 -DHAVE___BUILTIN_TYPES_COMPATIBLE_P=1 \
	-DHAVE_UINT128=1 -DHAVE_STRUCT_TIMESPEC=1

MESA_INC := -include $(MESA)/port/inc/_mesa_port_cfg.h \
	-I$(MESA)/port/inc -I$(MESA)/gen -I$(MESA)/gen/mapi -I$(MESA)/gen/glsl \
	-I$(MESA)/gen/gallium -I$(MESA)/gen/util -I$(MESA)/gen/shared-glapi \
	-I$(MESA)/src/util/format \
	-I$(MESA)/include -I$(MESA)/src -I$(MESA)/src/mesa -I$(MESA)/src/mesa/main \
	-I$(MESA)/src/mesa/program -I$(MESA)/src/mesa/state_tracker \
	-I$(MESA)/src/compiler -I$(MESA)/src/compiler/nir -I$(MESA)/src/compiler/glsl -I$(MESA)/src/compiler/spirv -I$(MESA)/gen/compiler/spirv \
	-I$(MESA)/src/gallium/include -I$(MESA)/src/gallium/auxiliary \
	-I$(MESA)/src/gallium/auxiliary/util -I$(MESA)/src/gallium/drivers \
	-I$(MESA)/src/mapi -I$(MESA)/src/util -I$(MESA)/src/mapi/glapi \
	-I$(MESA)/src/compiler/glsl/glcpp -I$(MESA)/gen/glcpp -I$(MESA)/gen/sha1 \
	-I$(MESA)/gen/program \
	-I$(MESA)/src/util/sha1 -I$(MESA)/src/util/blake3

CFLAGS := -c -std=gnu11 -O2 -msse2 -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -m64 -mcmodel=kernel -mno-red-zone -fno-asynchronous-unwind-tables \
	-fvisibility=hidden -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers

CXXFLAGS := -c -std=c++17 -O2 -msse2 -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -m64 -mcmodel=kernel -mno-red-zone -fno-exceptions -fno-rtti \
	-fvisibility=hidden -Wno-unused-parameter -Wno-unused-function \
	-Wno-invalid-offsetof -Wno-missing-field-initializers

GEN_HDRS := $(MESA)/gen/nir_opcodes.h $(MESA)/gen/nir_intrinsics.h \
	$(MESA)/gen/builtin_types.h $(MESA)/gen/mapi/api_exec_decl.h \
	$(MESA)/gen/glsl/ir_expression_operation.h $(MESA)/gen/glsl/glsl_parser.h

MAPI_GEN := $(MESA)/gen/mapi/api_exec_init.c $(MESA)/gen/mapi/glapi_gentable.c \
	$(MESA)/gen/mapi/marshal_generated0.c $(MESA)/gen/mapi/marshal_generated1.c \
	$(MESA)/gen/mapi/marshal_generated2.c $(MESA)/gen/mapi/marshal_generated3.c \
	$(MESA)/gen/mapi/marshal_generated4.c $(MESA)/gen/mapi/marshal_generated5.c \
	$(MESA)/gen/mapi/marshal_generated6.c $(MESA)/gen/mapi/marshal_generated7.c \
	$(MESA)/gen/mapi/unmarshal_table.c $(MESA)/gen/mapi/enums.c

EXTRA_C := $(MESA)/src/util/sha1/sha1.c \
	$(MESA)/src/compiler/glsl/glcpp/pp.c \
	$(MESA)/gen/glcpp/glcpp-parse.c $(MESA)/gen/glcpp/glcpp-lex.c \
	$(MESA)/gen/program/program_parse.tab.c $(MESA)/gen/program/lex.yy.c

OTHER_GEN := $(MESA)/gen/u_format_table.c $(MESA)/gen/nir_opcodes.c \
	$(MESA)/gen/nir_intrinsics.c $(MESA)/gen/nir_constant_expressions.c \
	$(MESA)/gen/builtin_types.c $(MESA)/gen/gallium/u_indices_gen.c \
	$(MESA)/gen/gallium/u_unfilled_gen.c $(MESA)/gen/gallium/u_tracepoints.c

SOURCES_C := $(wildcard $(MESA)/src/compiler/nir/*.c) \
	$(MESA)/src/compiler/glsl_types.c $(MESA)/src/compiler/shader_enums.c \
	$(wildcard $(MESA)/src/compiler/glsl/*.c) \
	$(wildcard $(MESA)/src/mesa/main/*.c) \
	$(wildcard $(MESA)/src/mesa/vbo/*.c) \
	$(wildcard $(MESA)/src/mesa/program/*.c) \
	$(wildcard $(MESA)/src/mesa/math/*.c) \
	$(wildcard $(MESA)/src/mesa/state_tracker/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/util/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/translate/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/indices/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/tgsi/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/draw/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/cso_cache/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/pipebuffer/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/tessellator/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/nir/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/renderonly/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/rtasm/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/driver_trace/*.c) \
	$(wildcard $(MESA)/src/gallium/auxiliary/driver_ddebug/*.c) \
	$(wildcard $(MESA)/src/util/format/*.c) \
	$(wildcard $(MESA)/src/gallium/drivers/softpipe/*.c) \
	$(wildcard $(MESA)/src/util/*.c) \
	$(MESA)/src/util/perf/u_trace.c \
	$(MESA)/src/util/blake3/blake3.c $(MESA)/src/util/blake3/blake3_dispatch.c \
	$(MESA)/src/util/blake3/blake3_portable.c \
	$(MESA)/gen/format_srgb.c $(MESA)/gen/nir_opt_algebraic.c \
	$(MESA)/gen/format_fallback.c $(MESA)/gen/gallium/tr_util.c \
	$(wildcard $(MESA)/src/compiler/spirv/*.c) \
	$(MESA)/gen/compiler/spirv/spirv_info.c $(MESA)/gen/compiler/spirv/vtn_gather_types.c \
	$(MESA)/src/mapi/core.c $(MESA)/src/mapi/glapi/libgl_public.c \
	$(MAPI_GEN) $(OTHER_GEN) $(EXTRA_C)

SKIP_CPP := main standalone standalone_scaffolding

SOURCES_CPP := $(filter-out $(addprefix $(MESA)/src/compiler/glsl/,$(addsuffix .cpp,$(SKIP_CPP))),$(wildcard $(MESA)/src/compiler/glsl/*.cpp)) \
	$(MESA)/gen/glsl/glsl_parser.cpp $(MESA)/gen/glsl/glsl_lexer.cpp \
	$(wildcard $(MESA)/src/mesa/main/*.cpp) \
	$(wildcard $(MESA)/src/mesa/state_tracker/*.cpp) \
	$(MESA)/src/util/texcompress_astc_luts.cpp \
	$(MESA)/src/util/texcompress_astc_luts_wrap.cpp

SKIP_NAMES := draw_llvm draw_pt_fetch_shade_pipeline_llvm draw_vs_llvm \
	lp_bld_arit lp_bld_assert lp_bld_bitarit lp_bld_const lp_bld_conv \
	lp_bld_coro lp_bld_flow lp_bld_format lp_bld_format_aos \
	lp_bld_format_aos_array lp_bld_format_float lp_bld_format_s3tc \
	lp_bld_format_srgb lp_bld_format_yuv lp_bld_gather lp_bld_init \
	lp_bld_intrinsics lp_bld_limits lp_bld_logic lp_bld_misc lp_bld_pack \
	lp_bld_printf lp_bld_quad lp_bld_sample lp_bld_sample_aos \
	lp_bld_sample_soa lp_bld_struct lp_bld_swizzle lp_bld_test lp_bld_tgsi \
	lp_bld_tgsi_action lp_bld_tgsi_info lp_bld_tgsi_soa lp_bld_type \
	lp_bld_utils lp_bld_debug lp_bld_jit \
	u_tests u_trace_gallium os_memory_debug os_time u_debug_stack_android \
	u_debug_symbol_cache u_debug_symbol_libbacktrace \
	anon_file u_sync_provider mesa_cache_db xmlconfig \
	os_misc u_dl u_thread rwlock cnd_monotonic memstream os_file parson \
	nir_to_tgsi_info renderonly sse_minmax vl_ \
	u_indices u_unfilled_indices \
	u_format_tests u_format_unpack_neon u_debug_symbol spirv2nir

F_C := $(foreach f,$(SOURCES_C),$(if $(filter $(SKIP_NAMES),$(basename $(notdir $(f)))),,$(f)))

OBJS_C := $(patsubst $(MESA)/%.c,$(OBJ)/%-c.o,$(F_C))
OBJS_CXX := $(patsubst $(MESA)/%.cpp,$(OBJ)/%-cpp.o,$(SOURCES_CPP))

OBJS := $(OBJS_C) $(OBJS_CXX)

all: $(BUILD)/libmesa.a $(BUILD)/libmesa_port.a

$(BUILD)/libmesa.a: $(OBJS)
	@mkdir -p $(BUILD)
	@$(PY) $(MESA)/mkar.py -dir $(OBJ) $@ --skip port

PORT_SRC := $(wildcard $(MESA)/port/src/*.c)
PORT_CPP := $(wildcard $(MESA)/port/src/*.cpp)
PORT_ASM := $(wildcard $(MESA)/port/src/*.S)
PORT_OBJ := $(patsubst $(MESA)/port/src/%.c,$(OBJ)/port/%.o,$(PORT_SRC)) \
	$(patsubst $(MESA)/port/src/%.cpp,$(OBJ)/port/%.cpp.o,$(PORT_CPP)) \
	$(patsubst $(MESA)/port/src/%.S,$(OBJ)/port/%.S.o,$(PORT_ASM))

$(BUILD)/libmesa_port.a: $(PORT_OBJ)
	@mkdir -p $(BUILD)
	@$(PY) $(MESA)/mkar.py $@ $(PORT_OBJ)

PORT_CFLAGS := $(CFLAGS) -w -DHAVE_PTHREAD=1 -DHAVE_STRUCT_TIMESPEC=1 \
	-I$(MESA)/src -I$(MESA)/src/util -I$(MESA)/src/gallium/drivers/softpipe

$(OBJ)/port/%.cpp.o: $(MESA)/port/src/%.cpp
	@mkdir -p $(dir $@)
	@$(HOST_CXX) $(CXXFLAGS) -w $(COMMON_DEFS) $(MESA_INC) $< -o $@ 2> $@.err || 		{ echo "FAIL $<"; grep -m3 'error:' $@.err; exit 1; }

$(OBJ)/port/%.o: $(MESA)/port/src/%.c
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(PORT_CFLAGS) $(COMMON_DEFS) $(MESA_INC) -I$(MESA)/../.. -I$(MESA)/../../lib \
		-I$(MESA)/../../drivers $< -o $@ 2> $@.err || \
		{ echo "FAIL $<"; grep -m3 'error:' $@.err; exit 1; }

$(OBJ)/port/%.S.o: $(MESA)/port/src/%.S
	@mkdir -p $(dir $@)
	@$(HOST_CC) -c -m64 $< -o $@ 2> $@.err || \
		{ echo "FAIL $<"; grep -m3 'error:' $@.err; exit 1; }

$(OBJ)/%-c.o: $(MESA)/%.c $(GEN_HDRS)
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(CFLAGS) $(COMMON_DEFS) $(MESA_INC) $< -o $@ 2> $@.err || \
		{ echo "FAIL $<"; grep -m3 'error:' $@.err; exit 1; }

$(OBJ)/src/mapi/core-c.o: $(MESA)/src/mapi/core.c $(GEN_HDRS)
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(CFLAGS) $(COMMON_DEFS) -DMAPI_MODE_SHARED_GLAPI=1 $(MESA_INC) $< -o $@ 2> $@.err || \
		{ echo "FAIL $<"; grep -m3 'error:' $@.err; exit 1; }

$(OBJ)/%-cpp.o: $(MESA)/%.cpp $(GEN_HDRS)
	@mkdir -p $(dir $@)
	@$(HOST_CXX) $(CXXFLAGS) $(COMMON_DEFS) $(MESA_INC) $< -o $@ 2> $@.err || \
		{ echo "FAIL $<"; grep -m3 'error:' $@.err; exit 1; }

clean:
	rm -rf $(OBJ) $(BUILD)
