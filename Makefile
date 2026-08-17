MAIN_FILE := axe

SRC_DIR := src
BUILD_DIR := .build

FLAGS := -std=c23 -I $(BUILD_DIR) -I $(SRC_DIR) $(shell pkg-config --cflags --libs xkbcommon wayland-client freetype2 fontconfig lua5.4)

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

PROTO_OBJS := $(patsubst protocol/%.xml, $(BUILD_DIR)/%-protocol.o, $(shell fd -e xml . protocol))
PROTO_HEADERS := $(patsubst protocol/%.xml, $(BUILD_DIR)/%-client-protocol.h, $(shell fd -e xml . protocol))

.PRECIOUS: $(BUILD_DIR)/%.o $(BUILD_DIR)/%.h $(BUILD_DIR)/%.c

$(BUILD_DIR)/$(MAIN_FILE): $(OBJS) $(PROTO_OBJS)
	$(CC) -o $@ $^ $(FLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(PROTO_HEADERS) | $(BUILD_DIR)
	$(CC) -c $(FLAGS) $< -o $@

$(BUILD_DIR)/%-protocol.o: $(BUILD_DIR)/%-protocol.c
	$(CC) -c $(FLAGS) $^ -o $@

$(BUILD_DIR)/%.h: 
	wayland-scanner client-header < $(patsubst $(BUILD_DIR)/%-client-protocol.h, protocol/%.xml, $@) > $@

$(BUILD_DIR)/%.c: 
	wayland-scanner private-code < $(patsubst $(BUILD_DIR)/%-protocol.c, protocol/%.xml, $@) > $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

.PHONY: build
build: $(BUILD_DIR) $(BUILD_DIR)/$(MAIN_FILE)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
