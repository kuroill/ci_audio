TOOLCHAIN_BIN := $(CURDIR)/tools/nuclei-gcc-9.2.0/gcc/bin
BUILD_TOOLS_BIN := $(CURDIR)/tools/build-tools/bin

export PATH := $(TOOLCHAIN_BIN);$(BUILD_TOOLS_BIN);$(PATH)

.PHONY: build clean

build:
	$(MAKE) -C projects/offline_asr_llm_aiot_iis_sample/project_file PROJECT_NAME=offline_asr_llm_aiot_iis_sample

clean:
	$(MAKE) -C projects/offline_asr_llm_aiot_iis_sample/project_file PROJECT_NAME=offline_asr_llm_aiot_iis_sample clean