# libarctmap.so — 테라스탈 몸 면 지도 생성 (게임 쪽 MiniFFI 가 이름으로 불러 씀)
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := arctmap
LOCAL_SRC_FILES := arctmap/tmap.cpp
LOCAL_CPPFLAGS := -std=c++17 -O3 -ffp-contract=off -fvisibility=hidden
LOCAL_CPP_FEATURES := exceptions
LOCAL_LDLIBS := -lm
include $(BUILD_SHARED_LIBRARY)
