
#CC := clang -ggdb -lpthread
CC := clang
#CXX := clang++ -std=c++11 -ggdb -lpthread
CXX := clang++ -std=c++11 -lwiringPi
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

INVERTER_CPP := inverter.cpp

SERVER_CPP := server.cpp phttp/phttp.cpp
SERVER_C := phttp/sha1.c phttp/http11/http11_parser.c

SERVER_OBJ = $(patsubst %.cpp, $(OUT)/%$(OBJ), $(SERVER_CPP)) $(patsubst %.c, $(OUT)/%$(OBJ), $(SERVER_C))
INVERTER_OBJ = $(patsubst %.cpp, $(OUT)/%$(OBJ), $(INVERTER_CPP))

$(OUT)/%$(OBJ): %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXX_OBJ_OUT)$@ -c $<

$(OUT)/%$(OBJ): %.c
	@mkdir -p $(@D)
	$(CC) $(CC_OBJ_OUT)$@ -c $<

$(OUT)/server$(EXE): $(SERVER_OBJ)
	$(CXX) $(CXX_EXE_OUT)$@ $(SERVER_OBJ)

$(OUT)/inverter$(EXE): $(INVERTER_OBJ)
	$(CXX) $(CXX_EXE_OUT)$@ $(INVERTER_OBJ)
