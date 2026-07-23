# 独立 fuzz 构建 不碰主 CMake / build-asan / build-tsan
# 只编解析器纯字节单元 挂 libFuzzer + ASan + UBSan
# 用法:
#   make            # 编所有 fuzz 目标
#   make run-req    # 建目标并跑请求解析器 fuzz(用 seeds/ 起步)
#   make repro-req CRASH=path   # 拿崩溃样本单跑复现
#   make clean

# 强制 clang++:libFuzzer 是 clang 专属,gcc 的 -fsanitize=fuzzer 不认
CXX      := clang++
REPO     := ../..
GW       := $(REPO)/src/gateway
GWINC    := $(GW)/include

CXXFLAGS := -std=c++20 -g -O1 -fno-omit-frame-pointer \
            -fsanitize=fuzzer,address,undefined \
            -fsanitize-address-use-after-scope \
            -I$(GWINC) -I$(GWINC)/gw_parser

# 请求解析器目标编译单元(全纯字节 无框架依赖)
# http_parse.cpp 同时定义 HttpRespParser 故连 httpclient_parser 一起编
REQ_SRCS := fuzz_req_parser.cc \
            $(GW)/http_parse.cpp \
            $(GW)/http_msg.cpp \
            $(GWINC)/gw_parser/http_parser.cpp \
            $(GWINC)/gw_parser/httpclient_parser.cpp

BINS := fuzz_req_parser

.PHONY: all clean run-req repro-req
all: $(BINS)

fuzz_req_parser: $(REQ_SRCS)
	$(CXX) $(CXXFLAGS) $(REQ_SRCS) -o $@

# 跑:优先用已积累的 corpus 目录 没有就用 seeds 起步 崩溃写到 crashes/
# artifact_prefix 末尾带 crash- 前缀:只让"崩溃"样本落进 crashes/,
# 不带斜杠结尾的目录会把 REDUCE 归约中间产物也倒进去,污染证据。
run-req: fuzz_req_parser
	@mkdir -p corpus crashes
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	./fuzz_req_parser -artifact_prefix=crashes/crash- \
	  -max_len=8192 -timeout=10 -rss_limit_mb=2048 \
	  corpus seeds

repro-req: fuzz_req_parser
	@test -n "$(CRASH)" || (echo "用法: make repro-req CRASH=crashes/crash-xxxx"; exit 1)
	ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
	./fuzz_req_parser "$(CRASH)"

clean:
	rm -f $(BINS)
