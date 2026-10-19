# libarcfog.so — 폭주 안개(Rogue Aura) 계산 (게임 쪽 MiniFFI 가 이름으로 불러 씀)
include $(CLEAR_VARS)
LOCAL_MODULE := arcfog
LOCAL_SRC_FILES := arcfog/arcfog.c
LOCAL_CFLAGS := -O2 -fvisibility=hidden
LOCAL_LDLIBS := -lm
include $(BUILD_SHARED_LIBRARY)
