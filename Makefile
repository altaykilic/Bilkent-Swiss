# Compiler and flags
CC = g++-15
CFLAGS = -Wall -O2
IFLAGS = -I$(IMGUI_DIR) -I$(IMGUI_BACKEND_DIR) -Iinclude -Iexternal/ImGuiFileDialog
LFLAGS = -lSDL2 -lsdl2main -lSDL2_image

# Directories
SRC_DIR = src
IMGUI_DIR = external/imgui
IMGUI_BACKEND_DIR = $(IMGUI_DIR)/backends
OBJ_DIR = .obj
BIN_DIR = bin

# Output executable
BIN = $(BIN_DIR)/main

# Source and object files
SRCS = $(wildcard $(SRC_DIR)/*.cpp) \
       $(IMGUI_BACKEND_DIR)/imgui_impl_sdl2.cpp \
       $(IMGUI_BACKEND_DIR)/imgui_impl_sdlrenderer2.cpp \
       $(wildcard $(IMGUI_DIR)/imgui*.cpp) \
	   external/ImGuiFileDialog/ImGuiFileDialog.cpp

OBJS = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(notdir $(SRCS)))

SOURCE_FILES = $(wildcard $(SRC_DIR)/*.cpp)
DELETE_FILES = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SOURCE_FILES))

# Dependency files
DEP_DIR = .deps
DEPS = $(patsubst %.cpp, $(DEP_DIR)/%.d, $(notdir $(SRCS)))

# Make sure the folders exist
.PHONY: all clean
all: $(BIN)

# Link all object files into the final executable
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) $(LFLAGS) -o $@

# Compile each .cpp file into a .o file
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR) $(DEP_DIR)
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(IMGUI_BACKEND_DIR)/%.cpp | $(OBJ_DIR) $(DEP_DIR)
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

$(OBJ_DIR)/ImGuiFileDialog.o: external/ImGuiFileDialog/ImGuiFileDialog.cpp | $(OBJ_DIR) $(DEP_DIR)
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(IMGUI_DIR)/%.cpp | $(OBJ_DIR) $(DEP_DIR)
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

# Automatically generate dependency files
$(DEP_DIR)/%.d: $(SRC_DIR)/%.cpp | $(DEP_DIR)
	@$(CC) $(DEPFLAGS) $< > $@

$(DEP_DIR)/%.d: $(IMGUI_BACKEND_DIR)/%.cpp | $(DEP_DIR)
	@$(CC) $(DEPFLAGS) $< > $@

$(DEP_DIR)/%.d: $(IMGUI_DIR)/%.cpp | $(DEP_DIR)
	@$(CC) $(DEPFLAGS) $< > $@

# Create directories if they don't exist
$(BIN_DIR) $(OBJ_DIR) $(DEP_DIR):
	@mkdir -p $@

# Clean up generated files
clean:
	rm $(BIN_DIR)/* $(DEP_DIR)/*.d $(DELETE_FILES) out.trf round.txt

cleanall:
	rm $(BIN_DIR)/* $(DEP_DIR)/*.d $(OBJ_DIR)/*.o out.trf round.txt
