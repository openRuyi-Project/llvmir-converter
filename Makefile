LLVM_VER ?= 22

llvmir-converter-$(LLVM_VER): llvmir-converter.cpp
	clang++-${LLVM_VER} -g -O2 llvmir-converter.cpp \
		-I /usr/include/llvm-$(LLVM_VER) \
		-I /usr/include/llvm-c-$(LLVM_VER) \
		-I /usr/lib64/llvm$(LLVM_VER)/include \
		-lLLVM-$(LLVM_VER) \
		-lstdc++ \
		-o $@

all: llvmir-converter-$(LLVM_VER)

check: llvmir-converter-$(LLVM_VER)
	rm -rf output
	./llvmir-converter-$(LLVM_VER) --version
	./llvmir-converter-$(LLVM_VER) --list-targets
	./llvmir-converter-$(LLVM_VER) --dry-run test/llvmir-bin/test_array_cmd
	./llvmir-converter-$(LLVM_VER) -o=output/short --dry-run test/llvmir-bin/test_cmd
	./llvmir-converter-$(LLVM_VER) --dry-run test/llvmir-bin/test_multi_cmd
	./llvmir-converter-$(LLVM_VER) --dry-run test/llvmir-bin/test_multiline_cmd
	./llvmir-converter-$(LLVM_VER) --dry-run -o=output/normal --pgo-output=output/pgo --pgo-profiles-output=output/pgo-profraw test/llvmir-bin/test_cmd
	! ./llvmir-converter-$(LLVM_VER) --dry-run -o=output/normal --pgo-output=output/pgo test/llvmir-bin/test_cmd
	! ./llvmir-converter-$(LLVM_VER) --dry-run -o=output --pgo-output=./output --pgo-profiles-output=pgo-profraw test/llvmir-bin/test_cmd
	./llvmir-converter-$(LLVM_VER) -t test/template.ll test/llvmir/libxx.so.1_cmd
	./llvmir-converter-$(LLVM_VER) -t test/template.ll test/llvmir-bin/test_cmd
	./llvmir-converter-$(LLVM_VER) -o=output/short -t test/template.ll test/llvmir-bin/test_array_cmd
	./llvmir-converter-$(LLVM_VER) -t test/template.ll test/llvmir-bin/test_array_cmd
	./llvmir-converter-$(LLVM_VER) -o=output/pgo-normal --pgo-output=output/pgo --pgo-profiles-output=output/pgo-profraw -t test/template.ll test/llvmir-bin/test_cmd
	test -f output/short/bin/test-array
	test -f output/pgo-normal/bin/test
	test -f output/pgo/bin/test
	test -d output/pgo-profraw
	@echo "Tests completed. Output files:"
	@ls -la output/lib/ output/bin/ output/short/bin/ output/pgo-normal/bin/ output/pgo/bin/

clean:
	rm -f llvmir-converter llvmir-converter-$(LLVM_VER) llvmir-converter-??
	rm -rf output

