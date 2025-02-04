CCOMPILER := clang
CXXCOMPILER := clang++

#CCOMPILER := gcc
#CXXCOMPILER := g++

# Debug
#CC := $(CCOMPILER) -g -O0
#CXX := $(CXXCOMPILER) -std=c++11 -g -O0

# Regular
CC := $(CCOMPILER) -O1
CXX := $(CXXCOMPILER) -std=c++11 -O1

LINK := $(CXXCOMPILER) -lpthread
CXX_EXE_OUT := -o  
CC_OBJ_OUT := -o  
CXX_OBJ_OUT := -o  
SERVER := server
UNIT := unit
OBJ := .o
EXE := 

# target directory
OUT := build

# With this, you can do "make print-VARIABLE" to dump the value of that variable
print-% : ; @echo $* = $($*)

QUERY_CPP := query.cpp server/inverter.cpp

SERVER_CPP := server/server.cpp server/http.cpp server/controller.cpp server/monitor.cpp server/monitorUtils.cpp server/commands.cpp server/inverter.cpp phttp/phttp.cpp
SERVER_C := phttp/sha1.c phttp/http11/http11_parser.c bcm2835/bcm2835.c

SERVER_OBJ = $(patsubst %.cpp, $(OUT)/%$(OBJ), $(SERVER_CPP)) $(patsubst %.c, $(OUT)/%$(OBJ), $(SERVER_C))
QUERY_OBJ = $(patsubst %.cpp, $(OUT)/%$(OBJ), $(QUERY_CPP))

$(OUT)/%$(OBJ): %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXX_OBJ_OUT)$@ -c $<

$(OUT)/%$(OBJ): %.c
	@mkdir -p $(@D)
	$(CC) $(CC_OBJ_OUT)$@ -c $<

$(OUT)/server/server$(EXE): $(SERVER_OBJ)
	$(LINK) $(CXX_EXE_OUT)$@ $(SERVER_OBJ)

$(OUT)/query$(EXE): $(QUERY_OBJ)
	$(LINK) $(CXX_EXE_OUT)$@ $(QUERY_OBJ)
